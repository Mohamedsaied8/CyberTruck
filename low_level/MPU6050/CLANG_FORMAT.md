# Code Formatting with Clang-Format

This project includes automated code formatting using `clang-format` integrated with CMake.

## Installation

If clang-format is not installed, install it with:

```bash
sudo apt-get install clang-format
```

## Available CMake Targets

### Format All Source Files

To format all source files according to the `.clang-format` configuration:

```bash
cmake --build build/Debug --target format
```

This will automatically format:
- All `.c` files in `Src/` and `Library/src/`
- All `.h` files in `Library/inc/`

### Check Formatting

To check if files need formatting without modifying them:

```bash
cmake --build build/Debug --target format-check
```

## Configuration

The formatting rules are defined in `.clang-format`. Key settings:

- **IndentWidth**: 4 spaces
- **TabWidth**: 4 spaces
- **UseTab**: ForIndentation (tabs for indentation)
- **ColumnLimit**: 100 characters
- **PointerAlignment**: Right (pointer * next to variable name)
- **BreakBeforeBraces**: Linux style
- **AlwaysBreakAfterReturnType**: TopLevel functions

## Editor Integration

Most modern IDEs and editors support clang-format:

### VS Code
Install the "Clang-Format" extension and configure it in `.vscode/settings.json`:

```json
{
  "[c]": {
    "editor.defaultFormatter": "xaver.clang-format",
    "editor.formatOnSave": true
  }
}
```

### Manual Formatting

To format a single file:

```bash
clang-format -i /path/to/file.c
```

To check formatting without modifying:

```bash
clang-format /path/to/file.c
```
