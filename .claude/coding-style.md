# Coding Style Preferences

## Brace Style: Allman (BSD Style)

Opening braces should be placed on their own line, aligned with the control statement.

### Examples

**Correct (Allman Style):**
```cpp
void function()
{
  if (condition)
  {
    statement;
  }
  else
  {
    other_statement;
  }

  for (int i = 0; i < 10; i++)
  {
    loop_body();
  }

  while (condition)
  {
    loop_body();
  }
}
```

**Incorrect (K&R Style - NOT to be used):**
```cpp
void function() {
  if (condition) {
    statement;
  } else {
    other_statement;
  }
}
```

## Why Allman Style?

1. **Vertical Alignment**: Opening braces align with their corresponding control statements, making block boundaries visually clear
2. **Easier to Spot Errors**: Missing braces are immediately obvious
3. **Symmetry**: Opening and closing braces are at the same indentation level
4. **Readability**: Each logical element (statement, opening brace, body, closing brace) has its own line

## Additional Style Guidelines

- **Indentation**: 2 spaces (no tabs)
- **Line Length**: Aim for 80-100 characters max
- **Comments**: Use profuse comments for learning code
- **Naming**:
  - Variables: `snake_case` or `camelCase`
  - Constants/Defines: `UPPER_CASE`
  - Functions: `snake_case` or `camelCase`
  - Classes: `PascalCase`

---

**Note to AI Assistant**: When generating or modifying code for this user, always use Allman brace style with opening braces on new lines, aligned with their control statements.
