find_package(absl CONFIG REQUIRED)

add_executable(gbt_flat_map_benchmark
    main.cpp
)

target_compile_features(gbt_flat_map_benchmark
    PRIVATE
        cxx_std_20
)

target_link_libraries(gbt_flat_map_benchmark
    PRIVATE
        gbt::flat_map
        absl::base
        absl::flat_hash_map
)
