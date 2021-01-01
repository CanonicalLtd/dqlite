#ifndef TEST_REGISTRY_H
#define TEST_REGISTRY_H

#include "../../src/registry.h"

#define FIXTURE_REGISTRY struct registry registry
#define SETUP_REGISTRY registryInit(&f->registry, &f->config)
#define TEAR_DOWN_REGISTRY registryClose(&f->registry);

#endif /* TEST_REGISTRY_H */
