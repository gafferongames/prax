# Mermaid: diagrams in Mermaid's language, parsed, laid out and drawn.
pr_library(mermaid INCLUDE include
    src/mermaid.cpp
    src/mermaid_class.cpp
    src/mermaid_class_draw.cpp
    src/mermaid_er.cpp
    src/mermaid_er_draw.cpp
    src/mermaid_flowchart.cpp
    src/mermaid_flowchart_draw.cpp
    src/mermaid_gantt.cpp
    src/mermaid_gantt_draw.cpp
    src/mermaid_gitgraph.cpp
    src/mermaid_journey.cpp
    src/mermaid_mindmap.cpp
    src/mermaid_parse.cpp
    src/mermaid_pie.cpp
    src/mermaid_quadrant.cpp
    src/mermaid_render.cpp
    src/mermaid_requirement.cpp
    src/mermaid_requirement_draw.cpp
    src/mermaid_sequence.cpp
    src/mermaid_sequence_draw.cpp
    src/mermaid_state.cpp
    src/mermaid_state_draw.cpp
    src/mermaid_timeline.cpp
    src/mermaid_xychart.cpp
)
target_link_libraries(pr_mermaid PUBLIC prax::core prax::color prax::draw prax::font prax::geom prax::text
                                 PRIVATE prax::core_cpp prax::graph prax::html_entity prax::markdown prax::path)

if (PR_DEVELOP)
    pr_module_tests(mermaid
        tests/test_mermaid.cpp
        tests/test_mermaid_c.c
        tests/test_mermaid_class.cpp
        tests/test_mermaid_er.cpp
        tests/test_mermaid_flowchart.cpp
        tests/test_mermaid_gantt.cpp
        tests/test_mermaid_gitgraph.cpp
        tests/test_mermaid_journey.cpp
        tests/test_mermaid_mindmap.cpp
        tests/test_mermaid_pie.cpp
        tests/test_mermaid_quadrant.cpp
        tests/test_mermaid_requirement.cpp
        tests/test_mermaid_sequence.cpp
        tests/test_mermaid_state.cpp
        tests/test_mermaid_xychart.cpp
        tests/test_mermaid_timeline.cpp
    )
    target_link_libraries(mermaid_tests PRIVATE prax::draw prax::font prax::graph prax::text)
    target_compile_definitions(mermaid_tests PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
endif()
