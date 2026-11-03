#pragma once

// Only the stable C ABI used by inference. Keeping these declarations private
// avoids requiring the training SDK's C++/Arrow headers at runtime/build time.
// Signatures/constants verified against LightGBM 4.6/4.7 C API:
// https://lightgbm.readthedocs.io/en/stable/C-API.html
#include <cstddef>
#include <cstdint>

extern "C" {
const char* LGBM_GetLastError();
int LGBM_BoosterCreateFromModelfile(const char*, int*, void**);
int LGBM_BoosterFree(void*);
int LGBM_BoosterGetNumFeature(void*, int*);
int LGBM_BoosterGetNumClasses(void*, int*);
int LGBM_BoosterGetFeatureNames(void*, int, int*, std::size_t, std::size_t*, char**);
int LGBM_BoosterPredictForMat(void*, const void*, int, std::int32_t, std::int32_t,
                            int, int, int, int, const char*, std::int64_t*, double*);
}

namespace TS::LightGbmApi {
inline constexpr int Float64 = 1;
inline constexpr int NormalPrediction = 0;
}
