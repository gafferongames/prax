# Graphs: directed graphs of sized nodes, ports and groups, and their layered layout.
pr_library(graph INCLUDE include
    src/graph.cpp
    src/graph_layered.cpp
    src/graph_order.cpp
    src/graph_place.cpp
    src/graph_place_balanced.cpp
    src/graph_route.cpp
    src/graph_simplex.cpp
)
target_link_libraries(pr_graph PUBLIC prax::core prax::geom PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(graph
        tests/test_graph.cpp
        tests/test_graph_c.c
        tests/test_graph_order.cpp
        tests/test_graph_simplex.cpp
    )

    # What laying out generated graphs costs, whole and by phase, which ./dev.sh bench graph runs.
    pr_harness(graph_bench prax::graph tests/graph_bench.cpp)
    target_link_libraries(graph_bench PRIVATE prax::core_cpp)
    target_include_directories(graph_bench PRIVATE ${PROJECT_SOURCE_DIR}/lib)
endif()
