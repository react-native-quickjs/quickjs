/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Generates a config class and its Builder from a field list, the way Hermes
 * does. Same shape as upstream -- getX, getDefaultX, rebuild, Builder::withX,
 * hasX, update, build -- so code written against Hermes compiles unchanged.
 */

#pragma once

#include <utility>

#define _HERMES_CTORCONFIG_FIELD(CX, TYPE, NAME, ...) TYPE NAME##_{__VA_ARGS__};
#define _HERMES_CTORCONFIG_EXPLICIT(CX, TYPE, NAME, ...) \
  bool NAME##Explicit_{false};

#define _HERMES_CTORCONFIG_GETTER(CX, TYPE, NAME, ...) \
  inline TYPE get##NAME() const {                      \
    return NAME##_;                                    \
  }                                                    \
  static CX TYPE getDefault##NAME() {                  \
    using TypeAsSingleToken = TYPE;                    \
    return TypeAsSingleToken{__VA_ARGS__};             \
  }

#define _HERMES_CTORCONFIG_SETTER(CX, TYPE, NAME, ...) \
  inline auto with##NAME(TYPE NAME)->decltype(*this) { \
    config_.NAME##_ = std::move(NAME);                 \
    NAME##Explicit_ = true;                            \
    return *this;                                      \
  }                                                    \
  bool has##NAME() const {                             \
    return NAME##Explicit_;                            \
  }

#define _HERMES_CTORCONFIG_UPDATE(CX, TYPE, NAME, ...) \
  if (newConfig.has##NAME()) with##NAME(newConfig.config_.get##NAME());

#define _HERMES_CTORCONFIG_STRUCT(NAME, FIELDS, BUILD_BODY)                    \
  class NAME {                                                                 \
    FIELDS(_HERMES_CTORCONFIG_FIELD)                                           \
                                                                               \
   public:                                                                     \
    class Builder;                                                             \
    friend Builder;                                                            \
    FIELDS(_HERMES_CTORCONFIG_GETTER)                                          \
    inline Builder rebuild() const;                                            \
                                                                               \
   private:                                                                    \
    inline void doBuild(const Builder &builder);                               \
  };                                                                           \
                                                                               \
  class NAME::Builder {                                                        \
    NAME config_;                                                              \
    FIELDS(_HERMES_CTORCONFIG_EXPLICIT)                                        \
                                                                               \
   public:                                                                     \
    Builder() = default;                                                       \
    explicit Builder(const NAME &config) : config_(config) {}                  \
    inline const NAME build() {                                                \
      config_.doBuild(*this);                                                  \
      return config_;                                                          \
    }                                                                          \
    inline Builder update(const NAME::Builder &newConfig);                     \
    FIELDS(_HERMES_CTORCONFIG_SETTER)                                          \
  };                                                                           \
                                                                               \
  inline NAME::Builder NAME::rebuild() const {                                 \
    return Builder(*this);                                                     \
  }                                                                            \
                                                                               \
  inline NAME::Builder NAME::Builder::update(const NAME::Builder &newConfig) { \
    FIELDS(_HERMES_CTORCONFIG_UPDATE)                                          \
    return *this;                                                              \
  }                                                                            \
                                                                               \
  inline void NAME::doBuild(const NAME::Builder &builder) {                    \
    (void)builder;                                                             \
    BUILD_BODY                                                                 \
  }

/// Marks a field whose default is not constexpr, as upstream does.
#define HERMES_NON_CONSTEXPR
