package pkg;

//- @GenericClass defines/binding GAbs
//- Class childof GAbs
//- GAbs.node/kind abs
//- GAbs param.0 TVar
//- @T defines/binding TVar
//- TVar.node/kind absvar
public class GenericClass<T> {

  public static void foo() {
    //- @GenericClass ref GAbs
    //- @String ref StringType
    //- @var defines/binding VarVar
    //- VarVar typed OType
    //- OType.node/kind tapp
    //- OType param.0 GAbs
    //- OType param.1 StringType
    GenericClass<String> var;
  }
}
