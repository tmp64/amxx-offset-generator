| C++                               | amxxType      | PDB | Dwarf |
|-----------------------------------|---------------|-----|-------|
| `struct X`                        | `structure`   | Y   | Y     |
| `void *`                          | `pointer`     | Y   | Y     |
| `class CBaseEntity *`             | `classptr`    | Y   | Y     |
| (function ptr)                    | `function`    | Y   | Y     |
| (member function ptr)             | `function`    | Y   | Y     |
| `struct entvars_t *`              | `entvars`     | Y   | Y     |
| `struct edict_t *`                | `edict`       | Y   | Y     |
| `char[N]`                         | `string`      | Y   | Y     |
| `char *`                          | `stringptr`   | Y   | Y     |
| `typedef unsigned int string_t`   | `stringint`   | Y*  | Y     |
| `class EHANDLE`                   | `ehandle`     | Y   | Y     |
| `class Vector`                    | `vector`      | Y   | Y     |

\* Variable names are hardcoded in the converter
