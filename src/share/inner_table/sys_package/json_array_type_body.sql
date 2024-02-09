CREATE OR REPLACE TYPE BODY JSON_ARRAY_T AS
  STATIC function parse(jsn VARCHAR2) return JSON_ARRAY_T;
  PRAGMA INTERFACE(c, JSON_ARRAY_PARSE);

  STATIC FUNCTION parse(jsn CLOB) return JSON_ARRAY_T;
  PRAGMA INTERFACE(c, JSON_ARRAY_PARSE);
  STATIC FUNCTION parse(jsn BLOB) return JSON_ARRAY_T;
  PRAGMA INTERFACE(c, JSON_ARRAY_PARSE);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T(jsn VARCHAR2) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T(jsn CLOB) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T(jsn BLOB) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T(o JSON_ELEMENT_T) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  CONSTRUCTOR FUNCTION JSON_ARRAY_T(o JSON_ARRAY_T) RETURN SELF AS RESULT;
  PRAGMA INTERFACE(c, JSON_ARRAY_CONSTRUCTOR);

  MEMBER FUNCTION get(SELF IN OUT NOCOPY JSON_ARRAY_T, pos IN NUMBER) return JSON_ELEMENT_T;
  PRAGMA INTERFACE(c, JSON_ARRAY_GET);

  MEMBER PROCEDURE on_Error(SELF IN OUT NOCOPY JSON_ARRAY_T, val IN NUMBER);
  PRAGMA INTERFACE(c, JSON_ARRAY_ON_ERROR);

  MEMBER FUNCTION get_Type(pos IN NUMBER) return VARCHAR2;
  PRAGMA INTERFACE(c, JSON_ARRAY_GET_TYPE);

  MEMBER FUNCTION to_String RETURN VARCHAR2;
  PRAGMA INTERFACE(c, JSON_TO_STRING);

  MEMBER FUNCTION get_Size  RETURN NUMBER;
  PRAGMA INTERFACE(c, JSON_GET_SIZE);

  MEMBER FUNCTION clone(SELF IN OUT NOCOPY JSON_ARRAY_T) RETURN JSON_ARRAY_T;
  PRAGMA INTERFACE(c, JSON_ARRAY_CLONE);
END;
//