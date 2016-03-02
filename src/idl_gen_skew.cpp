/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// independent from idl_parser, since this code is not needed for most clients

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

namespace flatbuffers {
namespace skew {

// Ensure that a type is prefixed with its namespace whenever it is used
// outside of its namespace.
static std::string WrapInNameSpace(const Definition &def) {
  std::string qualified_name;
  for (auto it = def.defined_namespace->components.begin();
           it != def.defined_namespace->components.end(); ++it) {
    qualified_name += *it + ".";
  }
  return qualified_name + def.name;
}

// Generate a documentation comment, if available.
static void GenComment(const std::vector<std::string> &dc,
                       std::string *code_ptr,
                       const char *indent = nullptr) {
  if (dc.empty()) {
    // Don't output empty comment blocks with 0 lines of comment content.
    return;
  }

  std::string &code = *code_ptr;
  for (auto it = dc.begin(); it != dc.end(); ++it) {
    code += '\n';
    if (indent) code += indent;
    code += "#" + *it;
  }
}

// Generate an enum declaration and an enum string lookup table.
static void GenEnum(EnumDef &enum_def, std::string *code_ptr) {
  if (enum_def.generated) return;
  std::string &code = *code_ptr;
  std::string name = WrapInNameSpace(enum_def);
  GenComment(enum_def.doc_comment, code_ptr);
  code += "\ntype " + name + " = int\n";
  code += "\nnamespace " + name + " {\n";
  for (auto it = enum_def.vals.vec.begin();
       it != enum_def.vals.vec.end(); ++it) {
    auto &ev = **it;
    if (!ev.doc_comment.empty()) {
      if (it != enum_def.vals.vec.begin()) {
        code += '\n';
      }
      GenComment(ev.doc_comment, code_ptr, "  ");
    }
    code += "  const " + ev.name + " = " + NumToString(ev.value);
    code += " as " + name + "\n";
  }
  code += "}\n";
}

static std::string GenType(const Type &type) {
  switch (type.base_type) {
    case BASE_TYPE_BOOL:
    case BASE_TYPE_CHAR: return "Int8";
    case BASE_TYPE_UTYPE:
    case BASE_TYPE_UCHAR: return "Uint8";
    case BASE_TYPE_SHORT: return "Int16";
    case BASE_TYPE_USHORT: return "Uint16";
    case BASE_TYPE_INT: return "Int32";
    case BASE_TYPE_UINT: return "Uint32";
    case BASE_TYPE_LONG: return "Int64";
    case BASE_TYPE_ULONG: return "Uint64";
    case BASE_TYPE_FLOAT: return "Float32";
    case BASE_TYPE_DOUBLE: return "Float64";
    case BASE_TYPE_STRING: return "String";
    case BASE_TYPE_VECTOR: return GenType(type.VectorType());
    case BASE_TYPE_STRUCT: return type.struct_def->name;
    default: return "Table";
  }
}

static std::string GenGetter(const Type &type, const std::string &arguments) {
  switch (type.base_type) {
    case BASE_TYPE_STRING: return "bb.readString" + arguments;
    case BASE_TYPE_STRUCT: return "bb.readStruct" + arguments;
    case BASE_TYPE_UNION:  return "bb.readUnion" + arguments;
    case BASE_TYPE_VECTOR: return GenGetter(type.VectorType(), arguments);
    default: {
      auto getter = "bb.read" + MakeCamel(GenType(type)) + arguments;
      if (type.base_type == BASE_TYPE_BOOL) {
        getter += " as bool";
      } else if (type.enum_def) {
        getter += " as " + WrapInNameSpace(*type.enum_def);
      }
      return getter;
    }
  }
}

static std::string GenDefaultValue(const Value &value) {
  if (value.type.enum_def) {
    if (auto val = value.type.enum_def->ReverseLookup(
        atoi(value.constant.c_str()), false)) {
      return WrapInNameSpace(*value.type.enum_def) + "." + val->name;
    }
  }

  switch (value.type.base_type) {
    case BASE_TYPE_BOOL:
      return value.constant == "0" ? "false" : "true";

    case BASE_TYPE_STRING:
      return "null";

    case BASE_TYPE_LONG:
    case BASE_TYPE_ULONG:
      if (value.constant != "0") {
        int64_t constant = std::atoll(value.constant.c_str());
        return "FlatBuffers.Long.new(" + NumToString((int32_t)constant) +
          ", " + NumToString((int32_t)(constant >> 32)) + ")";
      }
      return ".ZERO";

    default:
      return value.constant;
  }
}

enum struct InOut {
  IN,
  OUT,
};

static std::string GenTypeName(const Type &type, InOut inOut) {
  if (type.enum_def) {
    return WrapInNameSpace(*type.enum_def);
  }

  switch (type.base_type) {
    case BASE_TYPE_BOOL: return "bool";
    case BASE_TYPE_NONE:
    case BASE_TYPE_UTYPE:
    case BASE_TYPE_CHAR:
    case BASE_TYPE_UCHAR:
    case BASE_TYPE_SHORT:
    case BASE_TYPE_USHORT:
    case BASE_TYPE_INT:
    case BASE_TYPE_UINT:
    case BASE_TYPE_UNION: return "int";
    case BASE_TYPE_LONG:
    case BASE_TYPE_ULONG: return "FlatBuffers.Long";
    case BASE_TYPE_FLOAT:
    case BASE_TYPE_DOUBLE: return "double";
    case BASE_TYPE_VECTOR:
    case BASE_TYPE_STRING:
    case BASE_TYPE_STRUCT: {
      std::string name;
      switch (type.base_type) {
        case BASE_TYPE_VECTOR: name = "List<" + GenTypeName(type.VectorType(), InOut::OUT) + ">"; break;
        case BASE_TYPE_STRING: name = "string"; break;
        case BASE_TYPE_STRUCT: name = WrapInNameSpace(*type.struct_def); break;
        default: assert(false);
      }
      if (inOut == InOut::IN) {
        return "FlatBuffers.Offset<" + name + ">";
      }
      return name;
    }
  }
}

// Returns the method name for use with add/put calls.
static std::string GenWriteMethod(const Type &type) {
  // Forward to signed versions since unsigned versions don't exist
  switch (type.base_type) {
    case BASE_TYPE_UTYPE:
    case BASE_TYPE_UCHAR: return GenWriteMethod(Type(BASE_TYPE_CHAR));
    case BASE_TYPE_USHORT: return GenWriteMethod(Type(BASE_TYPE_SHORT));
    case BASE_TYPE_UINT: return GenWriteMethod(Type(BASE_TYPE_INT));
    case BASE_TYPE_ULONG: return GenWriteMethod(Type(BASE_TYPE_LONG));
    default: break;
  }

  return IsScalar(type.base_type)
    ? MakeCamel(GenType(type))
    : (IsStruct(type) ? "Struct" : "Offset");
}

static void GenStructArgs(const StructDef &struct_def,
                          std::string *arguments,
                          const std::string &nameprefix) {
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end(); ++it) {
    auto &field = **it;
    if (IsStruct(field.value.type)) {
      // Generate arguments for a struct inside a struct. To ensure names
      // don't clash, and to make it obvious these arguments are constructing
      // a nested struct, prefix the name with the field name.
      GenStructArgs(*field.value.type.struct_def, arguments,
                    nameprefix + field.name + "_");
    } else {
      *arguments += ", " + nameprefix + field.name + " ";
      *arguments += GenTypeName(field.value.type, InOut::IN);
    }
  }
}

static bool ShouldCastToInt(const Type &type) {
  return type.base_type == BASE_TYPE_BOOL || type.enum_def || !IsScalar(type.base_type);
}

static void GenStructBody(const StructDef &struct_def,
                          std::string *body,
                          const std::string &nameprefix) {
  *body += "    builder.prep(";
  *body += NumToString(struct_def.minalign) + ", ";
  *body += NumToString(struct_def.bytesize) + ")\n";

  for (auto it = struct_def.fields.vec.rbegin();
       it != struct_def.fields.vec.rend(); ++it) {
    auto &field = **it;
    if (field.padding) {
      *body += "    builder.pad(" + NumToString(field.padding) + ")\n";
    }
    if (IsStruct(field.value.type)) {
      // Generate arguments for a struct inside a struct. To ensure names
      // don't clash, and to make it obvious these arguments are constructing
      // a nested struct, prefix the name with the field name.
      GenStructBody(*field.value.type.struct_def, body,
                    nameprefix + field.name + "_");
    } else {
      *body += "    builder.write" + GenWriteMethod(field.value.type) + "(";
      *body += nameprefix + field.name;
      if (ShouldCastToInt(field.value.type)) {
        *body += " as int";
      }
      *body += ")\n";
    }
  }
}

// Generate an accessor struct with constructor for a flatbuffers struct.
static void GenStruct(const Parser &parser, StructDef &struct_def,
                      std::string *code_ptr) {
  if (struct_def.generated) return;
  std::string full_name = WrapInNameSpace(struct_def);
  std::string &code = *code_ptr;
  std::string static_code;

  // Start the type
  GenComment(struct_def.doc_comment, code_ptr);
  code += "\ntype " + full_name + " : int {";
  static_code += "\n  const NULL = 0 as " + full_name + "\n";

  // Generate a special accessor for the table that when used as the root of a
  // FlatBuffer
  if (!struct_def.fixed) {
    static_code += "\n  def getRootAs" + struct_def.name + "(bb FlatBuffers.ByteBuffer) " + struct_def.name + " {\n";
    static_code += "    return (bb.readInt32(bb.position) + bb.position) as " + struct_def.name + "\n";
    static_code += "  }\n";

    // Generate the identifier check method
    if (parser.root_struct_def_ == &struct_def &&
        !parser.file_identifier_.empty()) {
      static_code += "\n  def bufferHasIdentifier(bb FlatBuffers.ByteBuffer) bool {\n";
      static_code += "    return bb.hasIdentifier(\"" + parser.file_identifier_;
      static_code += "\")\n  }\n";
    }
  }

  // Emit field accessors
  for (auto it = struct_def.fields.vec.begin();
       it != struct_def.fields.vec.end(); ++it) {
    auto &field = **it;
    if (field.deprecated) continue;
    GenComment(field.doc_comment, code_ptr, "  ");
    auto offset_prefix = "    var offset = bb.readOffset(self as int, " +
      NumToString(field.value.offset) + ")\n    return offset != 0 ? ";

    // Emit a scalar field
    if (IsScalar(field.value.type.base_type) ||
        field.value.type.base_type == BASE_TYPE_STRING) {
      code += "\n  def " + MakeCamel(field.name, false) + "(bb FlatBuffers.ByteBuffer) " + GenTypeName(field.value.type, InOut::OUT) + " {\n";
      if (struct_def.fixed) {
        code += "    return " + GenGetter(field.value.type, "((self as int) + " + NumToString(field.value.offset) + ")") + "\n  }\n";
      } else {
        code += offset_prefix + GenGetter(field.value.type, "((self as int) + offset)") + " : " + GenDefaultValue(field.value);
        code += "\n  }\n";
      }
    }

    // Emit an object field
    else {
      switch (field.value.type.base_type) {
        case BASE_TYPE_STRUCT: {
          auto type = WrapInNameSpace(*field.value.type.struct_def);
          code += "\n  def " + MakeCamel(field.name, false) + "(bb FlatBuffers.ByteBuffer) " + type + " {\n";
          if (struct_def.fixed) {
            code += "    return ((self as int) + " + NumToString(field.value.offset) + ") as " + type + "\n";
          } else {
            code += offset_prefix;
            code += field.value.type.struct_def->fixed ? "((self as int) + offset)" : "bb.readIndirect((self as int) + offset)";
            code += " as " + type + " : .NULL\n";
          }
          code += "  }\n";
          break;
        }

        case BASE_TYPE_VECTOR: {
          auto vectortype = field.value.type.VectorType();
          auto vectortypename = GenTypeName(vectortype, InOut::OUT);
          auto inline_size = InlineSize(vectortype);
          auto index = "bb.readVector((self as int) + offset) + index * " + NumToString(inline_size);
          code += "\n  def " + MakeCamel(field.name, false) + "(bb FlatBuffers.ByteBuffer, index int) " + vectortypename + " {\n";
          if (vectortype.base_type == BASE_TYPE_STRUCT) {
            code += offset_prefix;
            code += vectortype.struct_def->fixed ? "(" + index + ")" : "bb.readIndirect(" + index + ")";
            code += " as " + vectortypename;
          } else {
            code += offset_prefix + GenGetter(vectortype, "(" + index + ")");
          }
          code += " : ";
          if (field.value.type.element == BASE_TYPE_BOOL) {
            code += "false";
          } else if (field.value.type.element == BASE_TYPE_LONG ||
              field.value.type.element == BASE_TYPE_ULONG) {
            code += ".ZERO";
          } else if (IsScalar(field.value.type.element)) {
            code += "0";
          } else if (field.value.type.element == BASE_TYPE_STRING) {
            code += "null";
          } else {
            code += ".NULL";
          }
          code += "\n  }\n";
          break;
        }

        case BASE_TYPE_UNION:
          code += "\n  def " + MakeCamel(field.name, false) + "(bb FlatBuffers.ByteBuffer) int {\n";
          code += offset_prefix + GenGetter(field.value.type, "((self as int) + offset)") + " : 0\n";
          code += "  }\n";
          break;

        default:
          assert(0);
      }
    }

    // Emit a length helper
    if (field.value.type.base_type == BASE_TYPE_VECTOR) {
      code += "\n  def " + MakeCamel(field.name, false) + "Length(bb FlatBuffers.ByteBuffer) int {\n" + offset_prefix;
      code += "bb.readVectorLength((self as int) + offset) : 0\n  }\n";
    }
  }

  // Emit a factory constructor
  if (struct_def.fixed) {
    std::string arguments;
    GenStructArgs(struct_def, &arguments, "");
    static_code += "\n  def create" + struct_def.name + "(builder FlatBuffers.Builder" + arguments + ") FlatBuffers.Offset<" + full_name + "> {\n";
    GenStructBody(struct_def, &static_code, "");
    static_code += "    return builder.offset as FlatBuffers.Offset<" + full_name + ">\n  }\n";
  } else {
    // Generate a method to start building a new object
    static_code += "\n  def start" + struct_def.name + "(builder FlatBuffers.Builder) {\n";
    static_code += "    builder.startObject(" + NumToString(struct_def.fields.vec.size()) + ")\n";
    static_code += "  }\n";

    // Generate a set of static methods that allow table construction
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto &field = **it;
      if (field.deprecated) continue;
      auto argname = MakeCamel(field.name, false);
      if (!IsScalar(field.value.type.base_type)) {
        argname += "Offset";
      }

      // Generate the field insertion method
      static_code += "\n  def add" + MakeCamel(field.name) + "(builder FlatBuffers.Builder, " + argname + " " + GenTypeName(field.value.type, InOut::IN) + ") {\n";
      static_code += "    builder.addField" + GenWriteMethod(field.value.type) + "(";
      static_code += NumToString(it - struct_def.fields.vec.begin()) + ", " + argname;
      if (ShouldCastToInt(field.value.type)) {
        static_code += " as int";
      }
      static_code += ", ";
      if (!IsScalar(field.value.type.base_type)) {
        static_code += "0";
      } else if (field.value.type.base_type == BASE_TYPE_BOOL) {
        static_code += field.value.constant;
      } else {
        static_code += GenDefaultValue(field.value);
        if (field.value.type.enum_def) {
          static_code += " as int";
        }
      }
      static_code += ")\n  }\n";

      if (field.value.type.base_type == BASE_TYPE_VECTOR) {
        auto vector_type = field.value.type.VectorType();
        auto alignment = InlineAlignment(vector_type);
        auto elem_size = InlineSize(vector_type);

        // Generate a method to create a vector from a JavaScript array
        if (!IsStruct(vector_type)) {
          static_code += "\n  def create" + MakeCamel(field.name) + "Vector(builder FlatBuffers.Builder, data List<";
          static_code += GenTypeName(vector_type, InOut::IN) + ">) " + GenTypeName(field.value.type, InOut::IN) + " {\n";
          static_code += "    builder.startVector(" + NumToString(elem_size) + ", data.count, " + NumToString(alignment) + ")\n";
          static_code += "    for i = data.count - 1; i >= 0; i-- {\n";
          static_code += "      builder.add" + GenWriteMethod(vector_type) + "(data[i]";
          if (ShouldCastToInt(vector_type)) {
            static_code += " as int";
          }
          static_code += ")\n";
          static_code += "    }\n";
          static_code += "    return builder.endVector as " + GenTypeName(field.value.type, InOut::IN) + "\n";
          static_code += "  }\n";
        }

        // Generate a method to start a vector, data to be added manually after
        static_code += "\n  def start" + MakeCamel(field.name) + "Vector(builder FlatBuffers.Builder, numElems int) {\n";
        static_code += "    builder.startVector(" + NumToString(elem_size) + ", numElems, " + NumToString(alignment) + ")\n";
        static_code += "  }\n";

        // Generate a method to end a vector
        static_code += "\n  def end" + MakeCamel(field.name) + "Vector(builder FlatBuffers.Builder) " + GenTypeName(field.value.type, InOut::IN) + "{\n";
        static_code += "    return builder.endVector as " + GenTypeName(field.value.type, InOut::IN) + "\n";
        static_code += "  }\n";
      }
    }

    // Generate a method to stop building a new object
    static_code += "\n  def end" + struct_def.name + "(builder FlatBuffers.Builder) FlatBuffers.Offset<" + full_name + "> {\n";
    static_code += "    var offset = builder.endObject\n";
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto &field = **it;
      if (!field.deprecated && field.required) {
        static_code += "    builder.requiredField(offset, ";
        static_code += NumToString(field.value.offset);
        static_code += ") # " + field.name + "\n";
      }
    }
    static_code += "    return offset as FlatBuffers.Offset<" + full_name + ">\n";
    static_code += "  }\n";

    // Generate the method to complete buffer construction
    if (parser.root_struct_def_ == &struct_def) {
      static_code += "\n  def finish" + struct_def.name + "Buffer(builder FlatBuffers.Builder, offset FlatBuffers.Offset<" + full_name + ">) {\n";
      static_code += "    builder.finish(offset as int, ";
      if (!parser.file_identifier_.empty()) {
        static_code += "\"" + parser.file_identifier_ + "\"";
      } else {
        static_code += "null";
      }
      static_code += ")\n  }\n";
    }
  }

  code += "}\n\nnamespace " + full_name + " {" + static_code + "}\n";
}

}  // namespace skew

// Iterate through all definitions we haven't generate code for (enums, structs,
// and tables) and output them to a single file.
std::string GenerateSkew(const Parser &parser) {
  using namespace skew;

  // Generate code for all the enum declarations.
  std::string enum_code;
  for (auto it = parser.enums_.vec.begin();
       it != parser.enums_.vec.end(); ++it) {
    GenEnum(**it, &enum_code);
  }

  // Generate code for all structs, then all tables.
  std::string decl_code;
  for (auto it = parser.structs_.vec.begin();
       it != parser.structs_.vec.end(); ++it) {
    GenStruct(parser, **it, &decl_code);
  }

  // Only output file-level code if there were any declarations.
  if (enum_code.length() || decl_code.length()) {
    std::string code;
    code = "# This was automatically generated by the FlatBuffers compiler,"
           " do not modify\n";

    // Output the main declaration code from above.
    code += enum_code;
    code += decl_code;

    return code;
  }

  return std::string();
}

static std::string GeneratedFileName(const std::string &path,
                                     const std::string &file_name) {
  return path + file_name + "_generated.sk";
}

bool GenerateSkew(const Parser &parser,
                  const std::string &path,
                  const std::string &file_name) {
    auto code = GenerateSkew(parser);
    return !code.length() ||
           SaveFile(GeneratedFileName(path, file_name).c_str(), code, false);
}

std::string SkewMakeRule(const Parser &parser,
                         const std::string &path,
                         const std::string &file_name) {
  std::string filebase = flatbuffers::StripPath(
      flatbuffers::StripExtension(file_name));
  std::string make_rule = GeneratedFileName(path, filebase) + ": ";
  auto included_files = parser.GetIncludedFilesRecursive(file_name);
  for (auto it = included_files.begin();
       it != included_files.end(); ++it) {
    make_rule += " " + *it;
  }
  return make_rule;
}

}  // namespace flatbuffers
