#pragma once

#include <string>
#include <vector>

struct FileEntity {
    std::string file_id;   // relative path from repo root
    std::string file_name; // basename
    std::string path;      // absolute path
    std::string language;
    int loc;               // line count
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
    std::string function_id;   // parent_id::function_name
    std::string parent_id;
    std::string function_name;
    std::string parent_type;   // "file" or "class"
    int nesting_depth;
    int is_async;              // 0 or 1
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

struct ParseResult {
    FileEntity file;
    std::vector<ClassEntity> classes;
    std::vector<BaseClassEntity> base_classes;
    std::vector<FunctionEntity> functions;
    std::vector<ParamEntity> params;
    std::vector<LinkEntity> links;
};
