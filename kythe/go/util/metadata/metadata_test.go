/*
 * Copyright 2017 The Kythe Authors. All rights reserved.
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

package metadata

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"

	"github.com/golang/protobuf/proto"

	"kythe.io/kythe/go/test/testutil"
	"kythe.io/kythe/go/util/schema/edges"

	protopb "github.com/golang/protobuf/protoc-gen-go/descriptor"
	spb "kythe.io/kythe/proto/storage_go_proto"
)

func TestParse(t *testing.T) {
	tests := []struct {
		input string
		want  Rules
	}{
		// Minimal value: Just a plain type tag.
		{`{"type":"kythe0"}`, nil},

		// NOP values, multiple rules.
		{`{"type":"kythe0","meta":[
             {"type":"nop"},
             {"type":"nop","begin":42,"end":99}
          ]}`, Rules{
			{},
			{
				Begin: 42,
				End:   99,
			},
		}},

		// Test vector from the C++ implementation.
		{`{"type":"kythe0","meta":[{"type":"anchor_defines","begin":179,"end":182,
           "edge":"%/kythe/edge/generates",
           "vname":{
                 "signature":"gsig",
                 "corpus":"gcorp",
                 "path":"gpath",
                 "language":"glang",
                 "root":"groot"}
          }]}`, Rules{{
			Begin:   179,
			End:     182,
			EdgeIn:  edges.DefinesBinding,
			EdgeOut: "/kythe/edge/generates",
			Reverse: true,
			VName: &spb.VName{
				Signature: "gsig",
				Corpus:    "gcorp",
				Path:      "gpath",
				Language:  "glang",
				Root:      "groot",
			},
		}}},
	}
	for _, test := range tests {
		got, err := Parse(strings.NewReader(test.input))
		if err != nil {
			t.Errorf("Parse %q failed: %v", test.input, err)
			continue
		}

		if err := testutil.DeepEqual(test.want, got); err != nil {
			t.Errorf("Parse %q: %v", test.input, err)
		}
	}
}

func TestRoundTrip(t *testing.T) {
	tests := []Rules{
		nil,
		Rules{},
		Rules{{}},
		Rules{
			{},
			{Begin: 25, End: 37, EdgeOut: "blah"},
		},
		Rules{{
			VName: &spb.VName{
				Signature: "gsig",
				Corpus:    "gcorp",
				Path:      "gpath",
				Language:  "glang",
				Root:      "groot",
			},
			Reverse: true,
			EdgeIn:  edges.DefinesBinding,
			EdgeOut: edges.Generates,
			Begin:   179,
			End:     182,
		}},
	}
	for _, test := range tests {
		enc, err := json.Marshal(test)
		if err != nil {
			t.Errorf("Encoding %+v failed: %v", test, err)
			continue
		}

		dec, err := Parse(bytes.NewReader(enc))
		if err != nil {
			t.Errorf("Decoding %q failed: %v", string(enc), err)
			continue
		}

		if err := testutil.DeepEqual(test, dec); err != nil {
			t.Errorf("Round-trip of %+v failed: %v", test, err)
		}
	}
}

func TestGeneratedCodeInfo(t *testing.T) {
	in := &protopb.GeneratedCodeInfo{
		Annotation: []*protopb.GeneratedCodeInfo_Annotation{{
			Path:       []int32{1, 2, 3, 4, 5},
			SourceFile: proto.String("a"),
			Begin:      proto.Int(1),
			End:        proto.Int(100),
		}},
	}
	want := Rules{{
		VName: &spb.VName{
			Signature: "1.2.3.4.5",
			Language:  "protobuf",
			Path:      "a",
		},
		Reverse: true,
		EdgeIn:  edges.DefinesBinding,
		EdgeOut: edges.Generates,
		Begin:   1,
		End:     100,
	}}
	{
		got := FromGeneratedCodeInfo(in, nil)
		if err := testutil.DeepEqual(got, want); err != nil {
			t.Errorf("FromGeneratedCodeInfo failed: %v", err)
		}
	}
	{
		got := FromGeneratedCodeInfo(in, &spb.VName{
			Corpus: "blargh",
		})
		want[0].VName.Corpus = "blargh"
		if err := testutil.DeepEqual(got, want); err != nil {
			t.Errorf("FromGeneratedCodeInfo failed: %v", err)
		}
	}
}
