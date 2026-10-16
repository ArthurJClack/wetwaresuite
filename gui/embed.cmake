# Turns a binary file into a C++ source with an unsigned char array:
#   cmake -DINPUT=font.ttf -DOUTPUT=font.cpp -DNAME=kUiFont -DNAMESPACE=wetwaresuite::gui -P embed.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR bytes "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" body "${body}")
file(WRITE "${OUTPUT}" "// generated from ${INPUT}; do not edit\nnamespace ${NAMESPACE} {\nextern const unsigned char ${NAME}[];\nextern const unsigned int ${NAME}Size;\nconst unsigned char ${NAME}[] = {\n${body}\n};\nconst unsigned int ${NAME}Size = ${bytes};\n}\n")
