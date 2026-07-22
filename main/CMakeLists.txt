idf_component_register(SRCS "app_main.cpp" "splash.cpp" "state_display.cpp" "voice_pipeline.cpp"
                    INCLUDE_DIRS ".")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
