#pragma once

#include "crypto.h"
#include "diagnostics-channel.h"
#include "zlib-util.h"

#include <workerd/api/node/async-hooks.h>
#include <workerd/api/node/buffer.h>
#include <workerd/api/node/dns.h>
#include <workerd/api/node/module.h>
#include <workerd/api/node/process.h>
#include <workerd/api/node/timers.h>
#include <workerd/api/node/url.h>
#include <workerd/api/node/util.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

#include <node/node.capnp.h>

#include <capnp/dynamic.h>

namespace workerd::api::node {

// To be exposed only as an internal module for use by other built-ins.
// TODO(later): Consider moving out of node.h when needed for other
// built-ins
class CompatibilityFlags: public jsg::Object {
 public:
  CompatibilityFlags() = default;
  CompatibilityFlags(jsg::Lock&, const jsg::Url&) {}

  JSG_RESOURCE_TYPE(CompatibilityFlags, workerd::CompatibilityFlags::Reader flags) {
    // Not your typical JSG_RESOURCE_TYPE definition.. here we are iterating
    // through all of the compatibility flags and registering each as read-only
    // literal values on the instance...
    auto dynamic = capnp::toDynamic(flags);
    auto schema = dynamic.getSchema();
    for (auto field: schema.getFields()) {
      registry.template registerReadonlyInstanceProperty<bool>(
          field.getProto().getName(), dynamic.get(field).as<bool>());
    }
  }
};

#define NODEJS_MODULES(V)                                                                          \
  V(CompatibilityFlags, "workerd:compatibility-flags")                                             \
  V(AsyncHooksModule, "node-internal:async_hooks")                                                 \
  V(BufferUtil, "node-internal:buffer")                                                            \
  V(CryptoImpl, "node-internal:crypto")                                                            \
  V(ModuleUtil, "node-internal:module")                                                            \
  V(ProcessModule, "node-internal:process")                                                        \
  V(UtilModule, "node-internal:util")                                                              \
  V(DiagnosticsChannelModule, "node-internal:diagnostics_channel")                                 \
  V(ZlibUtil, "node-internal:zlib")                                                                \
  V(UrlUtil, "node-internal:url")                                                                  \
  V(DnsUtil, "node-internal:dns")                                                                  \
  V(TimersUtil, "node-internal:timers")

// Add to the NODEJS_MODULES_EXPERIMENTAL list any currently in-development
// node.js compat C++ modules that should be guarded by the experimental compat
// flag. Once they are ready to ship, move them up to the NODEJS_MODULES list.
#define NODEJS_MODULES_EXPERIMENTAL(V)

bool isNodeJsCompatEnabled(auto featureFlags) {
  return featureFlags.getNodeJsCompat() || featureFlags.getNodeJsCompatV2();
}

constexpr bool isExperimentalNodeJsCompatModule(kj::StringPtr name) {
  return name == "node:fs"_kj;
}

constexpr bool isNodeHttpModule(kj::StringPtr name) {
  return name == "node:http"_kj || name == "node:_http_common"_kj ||
      name == "node:_http_outgoing"_kj || name == "node:_http_client"_kj ||
      name == "node:_http_incoming"_kj || name == "node:_http_agent"_kj || name == "node:https"_kj;
}

template <class Registry>
void registerNodeJsCompatModules(Registry& registry, auto featureFlags) {
#define V(T, N)                                                                                    \
  registry.template addBuiltinModule<T>(N, workerd::jsg::ModuleRegistry::Type::INTERNAL);

  NODEJS_MODULES(V)

  if (featureFlags.getWorkerdExperimental()) {
    NODEJS_MODULES_EXPERIMENTAL(V)
  }

#undef V

  bool nodeJsCompatEnabled = isNodeJsCompatEnabled(featureFlags);

  registry.addBuiltinBundleFiltered(NODE_BUNDLE, [&](jsg::Module::Reader module) {
    // node:fs will be considered experimental until it's completed,
    // so unless the experimental flag is enabled, don't register them.
    if (isExperimentalNodeJsCompatModule(module.getName())) {
      return featureFlags.getWorkerdExperimental();
    }

    if (!nodeJsCompatEnabled) {
      // If the `nodejs_compat` flag isn't enabled, only register internal modules.
      // We need these for `console.log()`ing when running `workerd` locally.
      return module.getType() == jsg::ModuleType::INTERNAL;
    }

    // We put node:http and node:https modules behind a compat flag
    // for securing backward compatibility.
    if (isNodeHttpModule(module.getName())) {
      return featureFlags.getEnableNodejsHttpModules();
    }

    return true;
  });

  // If the `nodejs_compat` flag is off, but the `nodejs_als` flag is on, we
  // need to register the `node:async_hooks` module from the bundle.
  if (!nodeJsCompatEnabled && featureFlags.getNodeJsAls()) {
    jsg::Bundle::Reader reader = NODE_BUNDLE;
    for (auto module: reader.getModules()) {
      auto specifier = module.getName();
      if (specifier == "node:async_hooks") {
        KJ_DASSERT(module.getType() == jsg::ModuleType::BUILTIN);
        registry.addBuiltinModule(module);
      }
    }
  }
}

template <class TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalNodeJsCompatModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
#define V(M, N)                                                                                    \
  static const auto k##M##Specifier = N##_url;                                                     \
  builder.addObject<M, TypeWrapper>(k##M##Specifier);
  NODEJS_MODULES(V)
  if (featureFlags.getWorkerdExperimental()) {
    NODEJS_MODULES_EXPERIMENTAL(V)
  }
#undef V
  jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, NODE_BUNDLE);
  return builder.finish();
}

kj::Own<jsg::modules::ModuleBundle> getExternalNodeJsCompatModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
  if (isNodeJsCompatEnabled(featureFlags)) {
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, NODE_BUNDLE);
  } else if (featureFlags.getNodeJsAls()) {
    // The AsyncLocalStorage API can be enabled independently of the rest
    // of the nodejs_compat layer.
    jsg::Bundle::Reader reader = NODE_BUNDLE;
    for (auto module: reader.getModules()) {
      auto specifier = module.getName();
      if (specifier == "node:async_hooks") {
        KJ_DASSERT(module.getType() == jsg::ModuleType::BUILTIN);
        KJ_DASSERT(module.which() == workerd::jsg::Module::SRC);
        auto specifier = KJ_ASSERT_NONNULL(jsg::Url::tryParse(module.getName()));
        builder.addEsm(specifier, module.getSrc().asChars());
      }
    }
  }
  return builder.finish();
}

#undef NODEJS_MODULES
}  // namespace workerd::api::node

#define EW_NODE_ISOLATE_TYPES                                                                      \
  api::node::CompatibilityFlags, EW_NODE_BUFFER_ISOLATE_TYPES, EW_NODE_CRYPTO_ISOLATE_TYPES,       \
      EW_NODE_DIAGNOSTICCHANNEL_ISOLATE_TYPES, EW_NODE_ASYNCHOOKS_ISOLATE_TYPES,                   \
      EW_NODE_UTIL_ISOLATE_TYPES, EW_NODE_PROCESS_ISOLATE_TYPES, EW_NODE_ZLIB_ISOLATE_TYPES,       \
      EW_NODE_URL_ISOLATE_TYPES, EW_NODE_MODULE_ISOLATE_TYPES, EW_NODE_DNS_ISOLATE_TYPES,          \
      EW_NODE_TIMERS_ISOLATE_TYPES
