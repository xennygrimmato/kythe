package pkg;

public final class GenericMethodRef {
  public static final class Optional<T> {}

  //- @Optional ref OptionalClass
  //- @wildcard defines/binding WildcardFnAbs
  //- @ovar defines/binding WildcardParam1
  //- WildcardFnAbs.node/kind abs
  //- WildcardFnDecl childof WildcardFnAbs
  //- WildcardFnDecl param.0 WildcardParam1
  private static void wildcard(Optional<?> ovar) {}

  //- @verboseWildcard defines/binding VerboseWildcardFnAbs
  //- @ovar defines/binding VWParam1
  //- VerboseWildcardFnAbs.node/kind abs
  //- VerboseWildcardFn childof VerboseWildcardFnAbs
  //- VerboseWildcardFn.node/kind function
  //- VerboseWildcardFn param.0 VWParam1
  private static <T> void verboseWildcard(Optional<T> ovar) {}

  private static void caller() {
    //- @wildcard ref WildcardFnAbs
    //- @"wildcard(null)" ref/call WildcardFnAbs
    wildcard(null);

    //- @verboseWildcard ref VerboseWildcardFnAbs
    //- @"verboseWildcard(null)" ref/call VerboseWildcardFnAbs
    verboseWildcard(null);
  }
}
