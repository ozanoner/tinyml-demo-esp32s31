# Header-only component: the devkit abstraction's interface.
# Both the application (main) and every devkit component require this, so the
# interface lives outside any single devkit. It has no dependencies and pulls in
# no BSP, so it is harmless to have in every build.
idf_component_register(INCLUDE_DIRS "include")
