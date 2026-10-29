#include "LanguageRegistry.h"

#include "lang/python/PythonLanguage.h"

#include <utility>

void LanguageRegistry::add(std::unique_ptr<ILanguage> language)
{
    languages_.push_back(std::move(language));
}

const ILanguage* LanguageRegistry::find(const std::string& name) const
{
    for (const auto& lang : languages_)
        if (name == lang->name()) return lang.get();
    return nullptr;
}

const LanguageRegistry& LanguageRegistry::builtin()
{
    static const LanguageRegistry registry = [] {
        LanguageRegistry r;
        r.add(std::make_unique<PythonLanguage>());
        return r;
    }();
    return registry;
}
