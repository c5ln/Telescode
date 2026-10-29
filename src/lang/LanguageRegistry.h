#pragma once

#include "lang/ILanguage.h"

#include <memory>
#include <string>
#include <vector>

// ── Language registry ───────────────────────────────────────────────────────
// file.language -> the ILanguage that resolves that file's names. The resolver
// looks every file up here and leaves the refs of a language it does not find
// untouched ('pending').
class LanguageRegistry {
public:
    void add(std::unique_ptr<ILanguage> language);

    // nullptr if no plugin is registered under `name`.
    const ILanguage* find(const std::string& name) const;

    // Every language this build ships a plugin for.
    static const LanguageRegistry& builtin();

private:
    std::vector<std::unique_ptr<ILanguage>> languages_;
};
