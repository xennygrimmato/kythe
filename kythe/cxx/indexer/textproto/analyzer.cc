/*
 * Copyright 2019 The Kythe Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "analyzer.h"

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/types/optional.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/text_format.h"
#include "kythe/cxx/common/indexing/KytheGraphRecorder.h"
#include "kythe/cxx/common/path_utils.h"
#include "kythe/cxx/common/status_or.h"
#include "kythe/cxx/common/utf8_line_index.h"
#include "kythe/cxx/extractor/textproto/textproto_schema.h"
#include "kythe/cxx/indexer/proto/search_path.h"
#include "kythe/cxx/indexer/proto/source_tree.h"
#include "kythe/cxx/indexer/proto/vname_util.h"
#include "kythe/proto/analysis.pb.h"
#include "re2/re2.h"

namespace kythe {
namespace lang_textproto {

ABSL_CONST_INIT const absl::string_view kLanguageName = "textproto";

namespace {

using ::google::protobuf::Descriptor;
using ::google::protobuf::DescriptorPool;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;
using ::google::protobuf::TextFormat;

// Repeated fields have an actual index, non-repeated fields are always -1.
constexpr int kNonRepeatedFieldIndex = -1;

// Error "collector" that just writes messages to log output.
class LoggingMultiFileErrorCollector
    : public google::protobuf::compiler::MultiFileErrorCollector {
 public:
  void AddError(const std::string& filename, int line, int column,
                const std::string& message) override {
    LOG(ERROR) << filename << "@" << line << ":" << column << ": " << message;
  }

  void AddWarning(const std::string& filename, int line, int column,
                  const std::string& message) override {
    LOG(WARNING) << filename << "@" << line << ":" << column << ": " << message;
  }
};

absl::optional<proto::VName> LookupVNameForFullPath(
    absl::string_view full_path, const proto::CompilationUnit& unit) {
  for (const auto& input : unit.required_input()) {
    if (input.info().path() == full_path) {
      return input.v_name();
    }
  }
  return absl::nullopt;
}

// The TextprotoAnalyzer maintains state needed across indexing operations and
// provides some relevant helper methods.
class TextprotoAnalyzer {
 public:
  // Note: The TextprotoAnalyzer does not take ownership of its pointer
  // arguments, so they must outlive it.
  explicit TextprotoAnalyzer(
      const proto::CompilationUnit* unit, absl::string_view textproto,
      const absl::flat_hash_map<std::string, std::string>*
          file_substitution_cache,
      KytheGraphRecorder* recorder, const DescriptorPool* pool)
      : unit_(unit),
        recorder_(recorder),
        textproto_content_(textproto),
        line_index_(textproto),
        file_substitution_cache_(file_substitution_cache),
        descriptor_pool_(pool) {}

  // disallow copy and assign
  TextprotoAnalyzer(const TextprotoAnalyzer&) = delete;
  void operator=(const TextprotoAnalyzer&) = delete;

  // Recursively analyzes the message and any submessages, emitting "ref" edges
  // for all fields.
  absl::Status AnalyzeMessage(const proto::VName& file_vname,
                              const Message& proto,
                              const Descriptor& descriptor,
                              const TextFormat::ParseInfoTree& parse_tree);

  // Analyzes the message contained inside a google.protobuf.Any field. The
  // parse location of the field (if nonzero) is used to add an anchor for the
  // Any's type specifier (i.e. [some.url/mypackage.MyMessage]).
  absl::Status AnalyzeAny(const proto::VName& file_vname, const Message& proto,
                          const Descriptor& descriptor,
                          const TextFormat::ParseInfoTree& parse_tree,
                          TextFormat::ParseLocation field_loc);

  StatusOr<proto::VName> AnalyzeAnyTypeUrl(const proto::VName& file_vname,
                                           TextFormat::ParseLocation field_loc);

  absl::Status AnalyzeSchemaComments(const proto::VName& file_vname,
                                     const Descriptor& msg_descriptor);

  void EmitDiagnostic(const proto::VName& file_vname,
                      absl::string_view signature, absl::string_view msg);

 private:
  absl::Status AnalyzeField(const proto::VName& file_vname,
                            const Message& proto,
                            const TextFormat::ParseInfoTree& parse_tree,
                            const FieldDescriptor& field, int field_index);

  proto::VName CreateAndAddAnchorNode(const proto::VName& file, int begin,
                                      int end);

  absl::optional<proto::VName> VNameForRelPath(
      absl::string_view simplified_path) const;

  template <typename SomeDescriptor>
  StatusOr<proto::VName> VNameForDescriptor(const SomeDescriptor* descriptor) {
    absl::Status vname_lookup_status = absl::OkStatus();
    proto::VName vname = ::kythe::lang_proto::VNameForDescriptor(
        descriptor, [this, &vname_lookup_status](const std::string& path) {
          auto v = VNameForRelPath(path);
          if (!v.has_value()) {
            vname_lookup_status = absl::UnknownError(
                absl::StrCat("Unable to lookup vname for rel path: ", path));
            return proto::VName();
          }
          return *v;
        });
    return vname_lookup_status.ok() ? StatusOr<proto::VName>(vname)
                                    : vname_lookup_status;
  }

  const proto::CompilationUnit* unit_;
  KytheGraphRecorder* recorder_;
  const absl::string_view textproto_content_;
  const UTF8LineIndex line_index_;

  // Proto search paths are used to resolve relative paths to full paths.
  const absl::flat_hash_map<std::string, std::string>* file_substitution_cache_;
  // DescriptorPool is used to lookup descriptors for messages inside
  // protobuf.Any types.
  const DescriptorPool* descriptor_pool_;
};

absl::optional<proto::VName> TextprotoAnalyzer::VNameForRelPath(
    absl::string_view simplified_path) const {
  absl::string_view full_path;
  auto it = file_substitution_cache_->find(simplified_path);
  if (it != file_substitution_cache_->end()) {
    full_path = it->second;
  } else {
    full_path = simplified_path;
  }
  return LookupVNameForFullPath(full_path, *unit_);
}

absl::Status TextprotoAnalyzer::AnalyzeMessage(
    const proto::VName& file_vname, const Message& proto,
    const Descriptor& descriptor, const TextFormat::ParseInfoTree& parse_tree) {
  const Reflection* reflection = proto.GetReflection();

  // Iterate across all fields in the message. For proto1 and 2, each field has
  // a bit that tracks whether or not each field was set. This could be used to
  // only look at fields we know are set (with reflection.ListFields()). Proto3
  // however does not have "has" bits, so this approach would not work, thus we
  // look at every field.
  for (int field_index = 0; field_index < descriptor.field_count();
       field_index++) {
    const FieldDescriptor& field = *descriptor.field(field_index);
    if (field.is_repeated()) {
      // Handle repeated field.
      const int count = reflection->FieldSize(proto, &field);
      if (count == 0) {
        continue;
      }

      // Add a ref for each instance of the repeated field.
      for (int i = 0; i < count; i++) {
        auto s = AnalyzeField(file_vname, proto, parse_tree, field, i);
        if (!s.ok()) return s;
      }
    } else {
      auto s = AnalyzeField(file_vname, proto, parse_tree, field,
                            kNonRepeatedFieldIndex);
      if (!s.ok()) return s;
    }
  }

  // Determine what extensions are present in the parsed proto and analyze them.
  std::vector<const FieldDescriptor*> set_fields;
  reflection->ListFields(proto, &set_fields);
  for (const FieldDescriptor* field : set_fields) {
    // Non-extensions are already handled above.
    if (!field->is_extension()) {
      continue;
    }

    if (field->is_repeated()) {
      const size_t count = reflection->FieldSize(proto, field);
      for (size_t i = 0; i < count; i++) {
        auto s = AnalyzeField(file_vname, proto, parse_tree, *field, i);
        if (!s.ok()) return s;
      }
    } else {
      auto s = AnalyzeField(file_vname, proto, parse_tree, *field,
                            kNonRepeatedFieldIndex);
      if (!s.ok()) return s;
    }
  }

  return absl::OkStatus();
}

// Given a type url that looks like "type.googleapis.com/example.Message1",
// returns "example.Message1".
std::string ProtoMessageNameFromAnyTypeUrl(absl::string_view type_url) {
  // Return the substring from after the last '/' to the end or an empty string.
  // If there is no slash, returns the entire string.
  return std::string(
      type_url.substr(std::min(type_url.size(), type_url.rfind('/') + 1)));
}

// Example textproto:
//   any_field {
//     [some.url/mypackage.MyMessage] {
//     }
//   }
//
// Given the start location of "any_field" as field_loc, this function uses a
// regex to find the "mypackage.MyMessage" portion and add an anchor node.
// Ideally this information would be provided in the ParseInfoTree generated by
// the textproto parser, but since it's not, we do our own "parsing" with a
// regex.
StatusOr<proto::VName> TextprotoAnalyzer::AnalyzeAnyTypeUrl(
    const proto::VName& file_vname, TextFormat::ParseLocation field_loc) {
  // Note that line is 1-indexed; a value of zero indicates an empty location.
  if (field_loc.line == 0) return absl::OkStatus();

  re2::StringPiece sp(textproto_content_.data(), textproto_content_.size());
  const int search_from =
      line_index_.ComputeByteOffset(field_loc.line, field_loc.column);
  sp = sp.substr(search_from);

  // Consume rest of field name, colon (optional) and open brace.
  if (!re2::RE2::Consume(&sp, R"(^[a-zA-Z0-9_]+:?\s*\{\s*)")) {
    return absl::UnknownError("");
  }
  // consume any extra comments before "[type_url]".
  while (re2::RE2::Consume(&sp, R"(\s*#.*\n*)")) {
  }
  // Regex for Any type url enclosed by square brackets, capturing just the
  // message name.
  re2::StringPiece match;
  if (!re2::RE2::PartialMatch(sp, R"(^\s*\[\s*[^/]+/([^\s\]]+)\s*\])",
                              &match)) {
    return absl::UnknownError("Unable to find type_url span for Any");
  }

  // Add anchor.
  const int begin = match.begin() - textproto_content_.begin();
  const int end = begin + match.size();
  proto::VName anchor_vname = CreateAndAddAnchorNode(file_vname, begin, end);

  return anchor_vname;
}

// When the textproto parser finds an Any message in the input, it parses the
// contained message and serializes it into an Any message. The any has a
// 'type_url' field describing the message type and a 'value' field containing
// the serialized bytes of the message. To analyze, we create a new instance of
// the message based on the type_url and de-serialize the value bytes into it.
// This is then passed to AnalyzeMessage, which does the actual analysis and
// matches fields up with the ParseInfoTree.
absl::Status TextprotoAnalyzer::AnalyzeAny(
    const proto::VName& file_vname, const Message& proto,
    const Descriptor& descriptor, const TextFormat::ParseInfoTree& parse_tree,
    TextFormat::ParseLocation field_loc) {
  CHECK(descriptor.full_name() == "google.protobuf.Any");

  // Textproto usage of Any messages comes in two forms. You can specify the Any
  // directly via the `type_url` and `value` fields or you can specify the
  // message as a literal. If AnalyzeAnyTypeUrl() is unable to find a literal
  // starting with a type url enclosed in brackets, it returns an error and we
  // assume it's a directly-specified Any and defer to AnalyzeMessage.
  auto s = AnalyzeAnyTypeUrl(file_vname, field_loc);
  if (!s.ok()) {
    return AnalyzeMessage(file_vname, proto, descriptor, parse_tree);
  }
  const proto::VName type_url_anchor = *s;

  const FieldDescriptor* type_url_desc = descriptor.FindFieldByName("type_url");
  const FieldDescriptor* value_desc = descriptor.FindFieldByName("value");
  if (type_url_desc == nullptr || value_desc == nullptr) {
    return absl::UnknownError("Unable to get field descriptors for Any");
  }

  const Reflection* reflection = proto.GetReflection();

  std::string type_url = reflection->GetString(proto, type_url_desc);
  std::string msg_name = ProtoMessageNameFromAnyTypeUrl(type_url);
  const Descriptor* msg_desc =
      descriptor_pool_->FindMessageTypeByName(msg_name);
  if (msg_desc == nullptr) {
    // Log the error, but continue. Failure to include the descriptor for an Any
    // shouldn't stop the rest of the file from being indexed.
    LOG(ERROR) << "Unable to find descriptor for message named " << msg_name;
    return absl::OkStatus();
  }

  // Add ref from type_url to proto message.
  auto msg_vname = VNameForDescriptor(msg_desc);
  if (!msg_vname.ok()) {
    return msg_vname.status();
  }
  recorder_->AddEdge(VNameRef(type_url_anchor), EdgeKindID::kRef,
                     VNameRef(*msg_vname));

  // Deserialize Any value into the appropriate message type.
  std::string value_bytes = reflection->GetString(proto, value_desc);
  if (value_bytes.size() == 0) {
    // Any value is empty, nothing to index
    return absl::OkStatus();
  }
  google::protobuf::io::ArrayInputStream array_stream(value_bytes.data(),
                                                      value_bytes.size());
  google::protobuf::DynamicMessageFactory msg_factory;
  std::unique_ptr<Message> value_proto(
      msg_factory.GetPrototype(msg_desc)->New());
  google::protobuf::io::CodedInputStream coded_stream(&array_stream);
  if (!value_proto->ParseFromCodedStream(&coded_stream)) {
    return absl::UnknownError(absl::StrFormat(
        "Unable to parse Any.value bytes into a %s message", msg_name));
  }

  // Analyze the message contained in the Any.
  return AnalyzeMessage(file_vname, *value_proto, *msg_desc, parse_tree);
}

absl::Status TextprotoAnalyzer::AnalyzeField(
    const proto::VName& file_vname, const Message& proto,
    const TextFormat::ParseInfoTree& parse_tree, const FieldDescriptor& field,
    int field_index) {
  TextFormat::ParseLocation loc = parse_tree.GetLocation(&field, field_index);
  // GetLocation() returns 0-indexed values, but UTF8LineIndex expects
  // 1-indexed line numbers.
  loc.line++;

  bool add_anchor_node = true;
  if (loc.line == 0) {
    // When AnalyzeField() is called for repeated fields or extensions, we know
    // the field was actually present in the input textproto. In the case of
    // repeated fields, the presence of only one location entry but multiple
    // values indicates that the shorthand/inline repeated field syntax was
    // used. The inline syntax looks like:
    //
    //   repeated_field: ["value1", "value2"]
    //
    // Versus the standard syntax:
    //
    //   repeated_field: "value1"
    //   repeated_field: "value2"
    //
    // This case is handled specially because there is only one "repeated_field"
    // to add an anchor node for, but each value is still analyzed individually.
    if (field_index != kNonRepeatedFieldIndex && field_index > 0) {
      // Inline/short-hand repeated field syntax was used. There is no
      // "field_name:" for this entry to add an anchor node for.
      add_anchor_node = false;
    } else if (field.is_extension() || field_index != kNonRepeatedFieldIndex) {
      // If we can't find a location for a set extension or the first entry of
      // the repeated field, this is a bug.
      return absl::UnknownError(
          absl::StrCat("Failed to find location of field: ", field.full_name(),
                       ". This is a bug in the textproto indexer."));
    } else {
      // Normal proto field. Failure to find a location just means it's not set.
      return absl::OkStatus();
    }
  }

  if (add_anchor_node) {
    const size_t len =
        field.is_extension() ? field.full_name().size() : field.name().size();
    if (field.is_extension()) {
      loc.column++;  // Skip leading "[" for extensions.
    }
    const int begin = line_index_.ComputeByteOffset(loc.line, loc.column);
    const int end = begin + len;
    proto::VName anchor_vname = CreateAndAddAnchorNode(file_vname, begin, end);

    // Add ref to proto field.
    auto field_vname = VNameForDescriptor(&field);
    if (!field_vname.ok()) return field_vname.status();
    recorder_->AddEdge(VNameRef(anchor_vname), EdgeKindID::kRef,
                       VNameRef(*field_vname));
  }

  // Handle submessage.
  if (field.type() == FieldDescriptor::TYPE_MESSAGE) {
    const TextFormat::ParseInfoTree& subtree =
        *parse_tree.GetTreeForNested(&field, field_index);
    const Reflection* reflection = proto.GetReflection();
    const Message& submessage =
        field_index == kNonRepeatedFieldIndex
            ? reflection->GetMessage(proto, &field)
            : reflection->GetRepeatedMessage(proto, &field, field_index);
    const Descriptor& subdescriptor = *field.message_type();

    if (subdescriptor.full_name() == "google.protobuf.Any") {
      // The location of the field is used to find the location of the Any type
      // url and add an anchor node.
      TextFormat::ParseLocation field_loc =
          add_anchor_node ? loc : TextFormat::ParseLocation{};
      return AnalyzeAny(file_vname, submessage, subdescriptor, subtree,
                        field_loc);
    } else {
      return AnalyzeMessage(file_vname, submessage, subdescriptor, subtree);
    }
  }

  return absl::OkStatus();
}

absl::Status TextprotoAnalyzer::AnalyzeSchemaComments(
    const proto::VName& file_vname, const Descriptor& msg_descriptor) {
  TextprotoSchema schema = ParseTextprotoSchemaComments(textproto_content_);

  // Handle 'proto-message' comment if present.
  if (!schema.proto_message.empty()) {
    size_t begin = schema.proto_message.begin() - textproto_content_.begin();
    size_t end = begin + schema.proto_message.size();
    proto::VName anchor = CreateAndAddAnchorNode(file_vname, begin, end);

    // Add ref edge to proto message.
    auto msg_vname = VNameForDescriptor(&msg_descriptor);
    if (!msg_vname.ok()) return msg_vname.status();
    recorder_->AddEdge(VNameRef(anchor), EdgeKindID::kRef,
                       VNameRef(*msg_vname));
  }

  // Handle 'proto-file' and 'proto-import' comments if present.
  std::vector<absl::string_view> proto_files = schema.proto_imports;
  if (!schema.proto_file.empty()) {
    proto_files.push_back(schema.proto_file);
  }
  for (const absl::string_view file : proto_files) {
    size_t begin = file.begin() - textproto_content_.begin();
    size_t end = begin + file.size();
    proto::VName anchor = CreateAndAddAnchorNode(file_vname, begin, end);

    // Add ref edge to file.
    auto v = VNameForRelPath(file);
    if (!v.has_value()) {
      return absl::UnknownError(
          absl::StrCat("Unable to lookup vname for rel path: ", file));
    }
    recorder_->AddEdge(VNameRef(anchor), EdgeKindID::kRef, VNameRef(*v));
  }

  return absl::OkStatus();
}

proto::VName TextprotoAnalyzer::CreateAndAddAnchorNode(
    const proto::VName& file_vname, int begin, int end) {
  proto::VName anchor = file_vname;
  anchor.set_language(std::string(kLanguageName));
  anchor.set_signature(absl::StrCat("@", begin, ":", end));

  recorder_->AddProperty(VNameRef(anchor), NodeKindID::kAnchor);
  recorder_->AddProperty(VNameRef(anchor), PropertyID::kLocationStartOffset,
                         begin);
  recorder_->AddProperty(VNameRef(anchor), PropertyID::kLocationEndOffset, end);

  return anchor;
}

void TextprotoAnalyzer::EmitDiagnostic(const proto::VName& file_vname,
                                       absl::string_view signature,
                                       absl::string_view msg) {
  proto::VName dn_vname = file_vname;
  dn_vname.set_signature(std::string(signature));
  recorder_->AddProperty(VNameRef(dn_vname), NodeKindID::kDiagnostic);
  recorder_->AddProperty(VNameRef(dn_vname), PropertyID::kDiagnosticMessage,
                         msg);

  recorder_->AddEdge(VNameRef(file_vname), EdgeKindID::kTagged,
                     VNameRef(dn_vname));
}

// Find and return the argument after --proto_message. Removes the flag and
// argument from @args if found.
absl::optional<std::string> ParseProtoMessageArg(
    std::vector<std::string>* args) {
  for (size_t i = 0; i < args->size(); i++) {
    if (args->at(i) == "--proto_message") {
      if (i + 1 < args->size()) {
        std::string v = args->at(i + 1);
        args->erase(args->begin() + i, args->begin() + i + 2);
        return v;
      }
      return absl::nullopt;
    }
  }
  return absl::nullopt;
}

/// Given a full file path, returns a path relative to a directory in the
/// current search path. If the mapping isn't already in the cache, it is added.
/// \param full_path Full path to proto file
/// \param path_substitutions A map of (virtual directory, real directory) pairs
/// \param file_substitution_cache A map of (fullpath, relpath) pairs
std::string FullPathToRelative(
    const absl::string_view full_path,
    const std::vector<std::pair<std::string, std::string>>& path_substitutions,
    absl::flat_hash_map<std::string, std::string>* file_substitution_cache) {
  // If the SourceTree has opened this path already, its entry will be in the
  // cache.
  for (const auto& sub : *file_substitution_cache) {
    if (sub.second == full_path) {
      return sub.first;
    }
  }

  // Look through substitutions for a directory mapping that contains the given
  // full_path.
  // TODO(justbuchanan): consider using the *longest* match, not just the
  // first one.
  for (auto& sub : path_substitutions) {
    std::string dir = sub.second;
    if (!absl::EndsWith(dir, "/")) {
      dir += "/";
    }

    // If this substitution matches, apply it and return the simplified path.
    absl::string_view relpath = full_path;
    if (absl::ConsumePrefix(&relpath, dir)) {
      std::string result = sub.first.empty() ? std::string(relpath)
                                             : JoinPath(sub.first, relpath);
      (*file_substitution_cache)[result] = std::string(full_path);
      return result;
    }
  }

  return std::string(full_path);
}

}  // namespace

absl::Status AnalyzeCompilationUnit(const proto::CompilationUnit& unit,
                                    const std::vector<proto::FileData>& files,
                                    KytheGraphRecorder* recorder) {
  if (unit.source_file().size() != 1) {
    return absl::FailedPreconditionError(
        "Expected Unit to contain 1 source file");
  }
  if (files.size() < 2) {
    return absl::FailedPreconditionError(
        "Must provide at least 2 files: a textproto and 1+ .proto files");
  }

  const std::string textproto_name = unit.source_file(0);

  // Parse path substitutions from arguments.
  absl::flat_hash_map<std::string, std::string> file_substitution_cache;
  std::vector<std::pair<std::string, std::string>> path_substitutions;
  std::vector<std::string> args;
  ::kythe::lang_proto::ParsePathSubstitutions(unit.argument(),
                                              &path_substitutions, &args);

  // Find --proto_message in args.
  std::string message_name;
  {
    auto opt_message_name = ParseProtoMessageArg(&args);
    if (!opt_message_name.has_value()) {
      return absl::UnknownError(
          "Compilation unit arguments must specify --proto_message");
    }
    message_name = *opt_message_name;
  }
  LOG(INFO) << "Proto message name: " << message_name;

  // Load all proto files into in-memory SourceTree.
  PreloadedProtoFileTree file_reader(&path_substitutions,
                                     &file_substitution_cache);
  std::vector<std::string> proto_filenames;
  const proto::FileData* textproto_file_data = nullptr;
  for (const auto& file : files) {
    // Skip textproto - only proto files go in the descriptor db.
    if (file.info().path() == textproto_name) {
      textproto_file_data = &file;
      continue;
    }

    VLOG(1) << "Added file to descriptor db: " << file.info().path();
    if (!file_reader.AddFile(file.info().path(), file.content())) {
      return absl::UnknownError("Unable to add file to SourceTree.");
    }
    proto_filenames.push_back(file.info().path());
  }
  if (textproto_file_data == nullptr) {
    return absl::NotFoundError("Couldn't find textproto source in file data.");
  }

  // Build proto descriptor pool with top-level protos.
  LoggingMultiFileErrorCollector error_collector;
  google::protobuf::compiler::Importer proto_importer(&file_reader,
                                                      &error_collector);
  for (const std::string& fname : proto_filenames) {
    // The proto importer gets confused if the same proto file is Import()'d
    // under two different file paths. For example, if subdir/some.proto is
    // imported as "subdir/some.proto" in one place and "some.proto" in another
    // place, the importer will see duplicate symbol definitions and fail. To
    // work around this, we use relative paths for importing because the
    // "import" statements in proto files are also relative to the proto
    // compiler search path. This ensures that the importer doesn't see the same
    // file twice under two different names.
    std::string relpath =
        FullPathToRelative(fname, path_substitutions, &file_substitution_cache);
    if (!proto_importer.Import(relpath)) {
      return absl::UnknownError("Error importing proto file: " + relpath);
    }
    VLOG(1) << "Added proto to descriptor pool: " << relpath;
  }
  const DescriptorPool* descriptor_pool = proto_importer.pool();

  // Get a descriptor for the top-level Message.
  const Descriptor* descriptor =
      descriptor_pool->FindMessageTypeByName(message_name);
  if (descriptor == nullptr) {
    return absl::NotFoundError(absl::StrCat(
        "Unable to find proto message in descriptor pool: ", message_name));
  }

  // Use reflection to create an instance of the top-level proto message.
  // note: msg_factory must outlive any protos created from it.
  google::protobuf::DynamicMessageFactory msg_factory;
  std::unique_ptr<Message> proto(msg_factory.GetPrototype(descriptor)->New());

  // Parse textproto into @proto, recording input locations to @parse_tree.
  TextFormat::ParseInfoTree parse_tree;
  {
    TextFormat::Parser parser;
    parser.WriteLocationsTo(&parse_tree);
    // Relax parser restrictions - even if the proto is partially ill-defined,
    // we'd like to analyze the parts that are good.
    parser.AllowPartialMessage(true);
    parser.AllowUnknownExtension(true);
    if (!parser.ParseFromString(textproto_file_data->content(), proto.get())) {
      return absl::UnknownError("Failed to parse text proto");
    }
  }

  // Emit file node.
  absl::optional<proto::VName> file_vname =
      LookupVNameForFullPath(textproto_name, unit);
  if (!file_vname.has_value()) {
    return absl::UnknownError(
        absl::StrCat("Unable to find vname for textproto: ", textproto_name));
  }
  recorder->AddProperty(VNameRef(*file_vname), NodeKindID::kFile);
  // Record source text as a fact.
  recorder->AddProperty(VNameRef(*file_vname), PropertyID::kText,
                        textproto_file_data->content());

  // Analyze!
  TextprotoAnalyzer analyzer(&unit, textproto_file_data->content(),
                             &file_substitution_cache, recorder,
                             descriptor_pool);

  absl::Status status =
      analyzer.AnalyzeSchemaComments(*file_vname, *descriptor);
  if (!status.ok()) {
    std::string msg =
        absl::StrCat("Error analyzing schema comments: ", status.ToString());
    LOG(ERROR) << msg << status;
    analyzer.EmitDiagnostic(*file_vname, "schema_comments", msg);
  }

  return analyzer.AnalyzeMessage(*file_vname, *proto, *descriptor, parse_tree);
}

}  // namespace lang_textproto
}  // namespace kythe
