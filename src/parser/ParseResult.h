#pragma once

#include <string>
#include <vector>

struct FileEntity {
    std::string file_id;   // relative path from repo root
    std::string file_name; // basename
    std::string language;
    int raw_loc;             // total line count (including blanks and comments)
    int logical_loc;         // non-blank, non-comment line count
    int is_generated;        // 1 if vendored/generated, 0 otherwise
    int   max_cyclomatic_complexity; // max CC across functions in this file
    double avg_cyclomatic_complexity; // avg CC across functions in this file
    int   max_block_depth;           // worst function's block nesting depth
    double avg_block_depth;          // avg block nesting depth across functions
    int   max_function_loc;          // largest function's line count
    double avg_function_loc;         // avg function line count
};

struct ClassEntity {
    std::string class_id;   // file_id::ClassName
    std::string file_id;
    std::string class_name;
    int start_line;         // 0-indexed
    int end_line;           // 0-indexed
};

struct BaseClassEntity {
    std::string class_id;
    std::string base_class_name;
    int ordinal; // 0-indexed
};

struct FunctionEntity {
    std::string function_id;
    std::string file_id;
    std::string class_id;
    std::string function_name;
    int nesting_depth;
    int is_async;              // 0 or 1
    int cyclomatic_complexity; // base 1 + branch count
    int max_block_depth;       // deepest control-flow nesting inside this function
    int loc;                   // raw line count (end_line - start_line + 1)
    int start_line;            // 0-indexed
    int end_line;              // 0-indexed
};

struct ParamEntity {
    std::string function_id;
    std::string param_name;
    int ordinal; // 0-indexed
};

struct LinkEntity {
    std::string source_id;
    std::string target_id;
    std::string link_type; // CALLS, INHERITS, IMPORTS, DECORATES
};

struct FieldEntity {
    std::string class_id;
    std::string field_name;
    char access; // '+' public  '-' private  '#' protected
};

struct ParseResult {
    FileEntity file;
    std::vector<ClassEntity> classes;
    std::vector<BaseClassEntity> base_classes;
    std::vector<FunctionEntity> functions;
    std::vector<ParamEntity> params;
    std::vector<LinkEntity> links;
    std::vector<FieldEntity> fields;
};
