; These patterns capture, for TypeScript and TSX, the parameters that tree-sitter-javascript's
; `highlights-params.scm` captures in an array or object pattern. The TypeScript grammars put each
; parameter's pattern in a `required_parameter` or `optional_parameter`, so that file does not
; compile against them. TypeScript's own query captures the parameter that is a plain identifier.

(required_parameter
  pattern: [
    (array_pattern
      (identifier) @variable.parameter)
    (object_pattern
      [
        (pair_pattern value: (identifier) @variable.parameter)
        (shorthand_property_identifier_pattern) @variable.parameter
      ])
  ])

(optional_parameter
  pattern: [
    (array_pattern
      (identifier) @variable.parameter)
    (object_pattern
      [
        (pair_pattern value: (identifier) @variable.parameter)
        (shorthand_property_identifier_pattern) @variable.parameter
      ])
  ])
