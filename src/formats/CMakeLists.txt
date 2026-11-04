target_sources(
  sosig_app
  PRIVATE body_html.c highlight.c html.c markdown.c org.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES body_html.h highlight.h html.h markdown.h org.h toml.h
)

target_link_libraries(
  sosig_app
  PRIVATE sosig_vendor_md4c
          sosig_vendor_org4c
          sosig_vendor_tree_sitter_c
          sosig_vendor_tree_sitter_bash
          sosig_vendor_tree_sitter_javascript
          sosig_vendor_tree_sitter_typescript
          sosig_vendor_tree_sitter_tsx
          sosig_vendor_tree_sitter
)

if(SOSIG_BUILD_TESTING)
  foreach(module IN ITEMS body_html html markdown org toml)
    sosig_add_unit_test(${module})
  endforeach()
  sosig_add_unit_test(highlight OS memory)
endif()
