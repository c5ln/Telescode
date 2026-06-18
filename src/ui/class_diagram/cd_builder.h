// src/ui/class_diagram/cd_builder.h
// DB → CDGraph conversion. Caller opens/closes the sqlite3 handle.

#pragma once
#include "cd_model.h"

struct sqlite3;

namespace TS {
    CDGraph BuildCDGraph(sqlite3* db);
}
