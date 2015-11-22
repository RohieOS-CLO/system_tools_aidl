/*
 * Copyright (C) 2015, The Android Open Source Project
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

#include "aidl.h"

#include <fcntl.h>
#include <iostream>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#endif

#include <base/strings.h>

#include "aidl_language.h"
#include "generate_cpp.h"
#include "generate_java.h"
#include "import_resolver.h"
#include "logging.h"
#include "options.h"
#include "os.h"
#include "type_cpp.h"
#include "type_java.h"
#include "type_namespace.h"

#ifndef O_BINARY
#  define O_BINARY  0
#endif

using android::base::Split;
using std::cerr;
using std::endl;
using std::map;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

namespace android {
namespace aidl {
namespace {

// The following are gotten as the offset from the allowable id's between
// android.os.IBinder.FIRST_CALL_TRANSACTION=1 and
// android.os.IBinder.LAST_CALL_TRANSACTION=16777215
const int kMinUserSetMethodId = 0;
const int kMaxUserSetMethodId = 16777214;

bool check_filename(const std::string& filename,
                    const std::string& package,
                    const std::string& name,
                    unsigned line) {
    const char* p;
    string expected;
    string fn;
    size_t len;
    char cwd[MAXPATHLEN];
    bool valid = false;

#ifdef _WIN32
    if (isalpha(filename[0]) && filename[1] == ':'
        && filename[2] == OS_PATH_SEPARATOR) {
#else
    if (filename[0] == OS_PATH_SEPARATOR) {
#endif
        fn = filename;
    } else {
        fn = getcwd(cwd, sizeof(cwd));
        len = fn.length();
        if (fn[len-1] != OS_PATH_SEPARATOR) {
            fn += OS_PATH_SEPARATOR;
        }
        fn += filename;
    }

    if (!package.empty()) {
        expected = package;
        expected += '.';
    }

    len = expected.length();
    for (size_t i=0; i<len; i++) {
        if (expected[i] == '.') {
            expected[i] = OS_PATH_SEPARATOR;
        }
    }

    expected.append(name, 0, name.find('.'));

    expected += ".aidl";

    len = fn.length();
    valid = (len >= expected.length());

    if (valid) {
        p = fn.c_str() + (len - expected.length());

#ifdef _WIN32
        if (OS_PATH_SEPARATOR != '/') {
            // Input filename under cygwin most likely has / separators
            // whereas the expected string uses \\ separators. Adjust
            // them accordingly.
          for (char *c = const_cast<char *>(p); *c; ++c) {
                if (*c == '/') *c = OS_PATH_SEPARATOR;
            }
        }
#endif

        // aidl assumes case-insensitivity on Mac Os and Windows.
#if defined(__linux__)
        valid = (expected == p);
#else
        valid = !strcasecmp(expected.c_str(), p);
#endif
    }

    if (!valid) {
        fprintf(stderr, "%s:%d interface %s should be declared in a file"
                " called %s.\n",
                filename.c_str(), line, name.c_str(), expected.c_str());
    }

    return valid;
}

bool check_filenames(const std::string& filename, const AidlDocumentItem* items) {
  if (! items)
    return true;

  if (items->item_type == INTERFACE_TYPE_BINDER) {
    const AidlInterface* c = reinterpret_cast<const AidlInterface*>(items);
    return check_filename(filename, c->GetPackage(), c->GetName(), c->GetLine());
  }

  bool success = true;

  for (const AidlParcelable* p = reinterpret_cast<const AidlParcelable*>(items);
       p; p = p->next)
    success &= check_filename(filename, p->GetPackage(), p->GetName(),
                              p->GetLine());

  return success;
}

bool gather_types(const std::string& filename,
                  const AidlDocumentItem* all_items,
                  TypeNamespace* types) {
  bool success = true;

  if (! all_items)
    return true;

  if (all_items->item_type == INTERFACE_TYPE_BINDER)
    return types->AddBinderType(reinterpret_cast<const AidlInterface *>(all_items), filename);

  for (const AidlParcelable* item =
       reinterpret_cast<const AidlParcelable *>(all_items);
       item; item = item->next) {
    success &= types->AddParcelableType(item, filename);
  }

  return success;
}

int check_types(const string& filename,
                const AidlInterface* c,
                TypeNamespace* types) {
  int err = 0;

  // Has to be a pointer due to deleting copy constructor. No idea why.
  map<string, const AidlMethod*> method_names;
  for (const auto& m : c->GetMethods()) {
    bool oneway = m->IsOneway() || c->IsOneway();

    if (!types->MaybeAddContainerType(m->GetType().GetName()) ||
        !types->IsValidReturnType(m->GetType(), filename)) {
      err = 1;  // return type is invalid
    }

    if (oneway && m->GetType().GetName() != "void") {
        cerr << filename << ":" << m->GetLine()
            << " oneway method '" << m->GetName() << "' cannot return a value"
            << endl;
        err = 1;
    }

    int index = 1;
    for (const auto& arg : m->GetArguments()) {
      if (!types->MaybeAddContainerType(arg->GetType().GetName()) ||
          !types->IsValidArg(*arg, index, filename)) {
        err = 1;
      }

      if (oneway && arg->IsOut()) {
        cerr << filename << ":" << m->GetLine()
            << " oneway method '" << m->GetName()
            << "' cannot have out parameters" << endl;
        err = 1;
      }
    }

    auto it = method_names.find(m->GetName());
    // prevent duplicate methods
    if (it == method_names.end()) {
      method_names[m->GetName()] = m.get();
    } else {
      cerr << filename << ":" << m->GetLine()
           << " attempt to redefine method " << m->GetName() << "," << endl
           << filename << ":" << it->second->GetLine()
           << "    previously defined here." << endl;
      err = 1;
    }
  }
  return err;
}

void generate_dep_file(const std::string& dep_file_name,
                       const std::string& input_file_name,
                       const std::string& output_file_name,
                       const std::vector<std::unique_ptr<AidlImport>>& imports,
                       const IoDelegate& io_delegate) {
  CodeWriterPtr writer = io_delegate.GetCodeWriter(dep_file_name);
  if (!writer) {
    cerr << "Could not open " << dep_file_name << endl;
    return;
  }


  writer->Write("%s: \\\n", output_file_name.c_str());
  writer->Write("  %s %s\n", input_file_name.c_str(),
                imports.empty() ? "" : "\\");

  bool first = true;
  for (const auto& import : imports) {
    if (! first) {
      writer->Write(" \\\n");
    }
    first = false;

    if (! import->GetFilename().empty()) {
      writer->Write("  %s", import->GetFilename().c_str());
    }
  }

  writer->Write(first ? "\n" : "\n\n");

  // Output "<input_aidl_file>: " so make won't fail if the input .aidl file
  // has been deleted, moved or renamed in incremental build.
  writer->Write("%s :\n", input_file_name.c_str());

  // Output "<imported_file>: " so make won't fail if the imported file has
  // been deleted, moved or renamed in incremental build.
  for (const auto& import : imports) {
    if (! import->GetFilename().empty()) {
      writer->Write("%s :\n", import->GetFilename().c_str());
    }
  }
}

string generate_outputFileName(const JavaOptions& options,
                               const AidlInterface& interface) {
    string name = interface.GetName();
    string package = interface.GetPackage();
    string result;

    // create the path to the destination folder based on the
    // interface package name
    result = options.output_base_folder_;
    result += OS_PATH_SEPARATOR;

    string packageStr = package;
    size_t len = packageStr.length();
    for (size_t i=0; i<len; i++) {
        if (packageStr[i] == '.') {
            packageStr[i] = OS_PATH_SEPARATOR;
        }
    }

    result += packageStr;

    // add the filename by replacing the .aidl extension to .java
    result += OS_PATH_SEPARATOR;
    result.append(name, 0, name.find('.'));
    result += ".java";

    return result;
}

int check_and_assign_method_ids(const char * filename,
                                const std::vector<std::unique_ptr<AidlMethod>>& items) {
    // Check whether there are any methods with manually assigned id's and any that are not.
    // Either all method id's must be manually assigned or all of them must not.
    // Also, check for duplicates of user set id's and that the id's are within the proper bounds.
    set<int> usedIds;
    bool hasUnassignedIds = false;
    bool hasAssignedIds = false;
    for (const auto& item : items) {
        if (item->HasId()) {
            hasAssignedIds = true;
            // Ensure that the user set id is not duplicated.
            if (usedIds.find(item->GetId()) != usedIds.end()) {
                // We found a duplicate id, so throw an error.
                fprintf(stderr,
                        "%s:%d Found duplicate method id (%d) for method: %s\n",
                        filename, item->GetLine(),
                        item->GetId(), item->GetName().c_str());
                return 1;
            }
            // Ensure that the user set id is within the appropriate limits
            if (item->GetId() < kMinUserSetMethodId ||
                    item->GetId() > kMaxUserSetMethodId) {
                fprintf(stderr, "%s:%d Found out of bounds id (%d) for method: %s\n",
                        filename, item->GetLine(),
                        item->GetId(), item->GetName().c_str());
                fprintf(stderr, "    Value for id must be between %d and %d inclusive.\n",
                        kMinUserSetMethodId, kMaxUserSetMethodId);
                return 1;
            }
            usedIds.insert(item->GetId());
        } else {
            hasUnassignedIds = true;
        }
        if (hasAssignedIds && hasUnassignedIds) {
            fprintf(stderr,
                    "%s: You must either assign id's to all methods or to none of them.\n",
                    filename);
            return 1;
        }
    }

    // In the case that all methods have unassigned id's, set a unique id for them.
    if (hasUnassignedIds) {
        int newId = 0;
        for (const auto& item : items) {
            item->SetId(newId++);
        }
    }

    // success
    return 0;
}

// TODO: Remove this in favor of using the YACC parser b/25479378
bool ParsePreprocessedLine(const string& line, string* decl,
                           vector<string>* package, string* class_name) {
  // erase all trailing whitespace and semicolons
  const size_t end = line.find_last_not_of(" ;\t");
  if (end == string::npos) {
    return false;
  }
  if (line.rfind(';', end) != string::npos) {
    return false;
  }

  decl->clear();
  string type;
  vector<string> pieces = Split(line.substr(0, end + 1), " \t");
  for (const string& piece : pieces) {
    if (piece.empty()) {
      continue;
    }
    if (decl->empty()) {
      *decl = std::move(piece);
    } else if (type.empty()) {
      type = std::move(piece);
    } else {
      return false;
    }
  }

  // Note that this logic is absolutely wrong.  Given a parcelable
  // org.some.Foo.Bar, the class name is Foo.Bar, but this code will claim that
  // the class is just Bar.  However, this was the way it was done in the past.
  //
  // See b/17415692
  size_t dot_pos = type.rfind('.');
  if (dot_pos != string::npos) {
    *class_name = type.substr(dot_pos + 1);
    *package = Split(type.substr(0, dot_pos), ".");
  } else {
    *class_name = type;
    package->clear();
  }

  return true;
}

}  // namespace

namespace internals {

bool parse_preprocessed_file(const IoDelegate& io_delegate,
                             const string& filename, TypeNamespace* types) {
  bool success = true;
  unique_ptr<LineReader> line_reader = io_delegate.GetLineReader(filename);
  if (!line_reader) {
    LOG(ERROR) << "cannot open preprocessed file: " << filename;
    success = false;
    return success;
  }

  string line;
  unsigned lineno = 1;
  for ( ; line_reader->ReadLine(&line); ++lineno) {
    if (line.empty() || line.compare(0, 2, "//") == 0) {
      // skip comments and empty lines
      continue;
    }

    string decl;
    vector<string> package;
    string class_name;
    if (!ParsePreprocessedLine(line, &decl, &package, &class_name)) {
      success = false;
      break;
    }

    if (decl == "parcelable") {
      AidlParcelable doc(new AidlQualifiedName(class_name, ""),
                         lineno, package);
      types->AddParcelableType(&doc, filename);
    } else if (decl == "interface") {
      auto temp = new std::vector<std::unique_ptr<AidlMethod>>();
      AidlInterface doc(class_name, lineno, "", false, temp, package);
      types->AddBinderType(&doc, filename);
    } else {
      success = false;
      break;
    }
  }
  if (!success) {
    LOG(ERROR) << filename << ':' << lineno
               << " malformed preprocessed file line: '" << line << "'";
  }

  return success;
}

AidlError load_and_validate_aidl(
    const std::vector<std::string> preprocessed_files,
    const std::vector<std::string> import_paths,
    const std::string& input_file_name,
    const IoDelegate& io_delegate,
    TypeNamespace* types,
    std::unique_ptr<AidlInterface>* returned_interface,
    std::vector<std::unique_ptr<AidlImport>>* returned_imports) {
  AidlError err = AidlError::OK;

  std::map<AidlImport*,std::unique_ptr<AidlDocumentItem>> docs;

  // import the preprocessed file
  for (const string& s : preprocessed_files) {
    if (!parse_preprocessed_file(io_delegate, s, types)) {
      err = AidlError::BAD_PRE_PROCESSED_FILE;
    }
  }
  if (err != AidlError::OK) {
    return err;
  }

  // parse the input file
  Parser p{io_delegate};
  if (!p.ParseFile(input_file_name)) {
    return AidlError::PARSE_ERROR;
  }

  AidlDocumentItem* parsed_doc = p.GetDocument();
  if (parsed_doc == nullptr) {
    return AidlError::PARSE_ERROR;
  }
  if (parsed_doc->item_type != INTERFACE_TYPE_BINDER) {
    LOG(ERROR) << "refusing to generate code from aidl file defining "
                  "parcelable";
    return AidlError::FOUND_PARCELABLE;
  }

  unique_ptr<AidlInterface> interface(
      reinterpret_cast<AidlInterface*>(parsed_doc));

  if (!check_filename(input_file_name.c_str(), interface->GetPackage(),
                      interface->GetName(), interface->GetLine()) ||
      !types->IsValidPackage(interface->GetPackage())) {
    LOG(ERROR) << "Invalid package declaration '" << interface->GetPackage()
               << "'";
    return AidlError::BAD_PACKAGE;
  }

  // parse the imports of the input file
  ImportResolver import_resolver{io_delegate, import_paths};
  for (auto& import : p.GetImports()) {
    if (types->HasType(import->GetNeededClass())) {
      // There are places in the Android tree where an import doesn't resolve,
      // but we'll pick the type up through the preprocessed types.
      // This seems like an error, but legacy support demands we support it...
      continue;
    }
    string import_path = import_resolver.FindImportFile(import->GetNeededClass());
    if (import_path.empty()) {
      cerr << import->GetFileFrom() << ":" << import->GetLine()
           << ": couldn't find import for class "
           << import->GetNeededClass() << endl;
      err = AidlError::BAD_IMPORT;
      continue;
    }
    import->SetFilename(import_path);

    Parser p{io_delegate};
    if (!p.ParseFile(import->GetFilename())) {
      cerr << "error while parsing import for class "
           << import->GetNeededClass() << endl;
      err = AidlError::BAD_IMPORT;
      continue;
    }

    AidlDocumentItem* document = p.GetDocument();
    if (!check_filenames(import->GetFilename(), document))
      err = AidlError::BAD_IMPORT;
    docs[import.get()] = std::unique_ptr<AidlDocumentItem>(document);
  }
  if (err != AidlError::OK) {
    return err;
  }

  // gather the types that have been declared
  if (!gather_types(input_file_name.c_str(), parsed_doc, types)) {
    err = AidlError::BAD_TYPE;
  }
  for (const auto& import : p.GetImports()) {
    if (!gather_types(import->GetFilename(), docs[import.get()].get(), types)) {
      err = AidlError::BAD_TYPE;
    }
  }

  // check the referenced types in parsed_doc to make sure we've imported them
  if (check_types(input_file_name, interface.get(), types) != 0) {
    err = AidlError::BAD_TYPE;
  }
  if (err != AidlError::OK) {
    return err;
  }


  // assign method ids and validate.
  if (check_and_assign_method_ids(input_file_name.c_str(),
                                  interface->GetMethods()) != 0) {
    return AidlError::BAD_METHOD_ID;
  }

  if (returned_interface)
    *returned_interface = std::move(interface);

  if (returned_imports)
    p.ReleaseImports(returned_imports);

  return AidlError::OK;
}

} // namespace internals

int compile_aidl_to_cpp(const CppOptions& options,
                        const IoDelegate& io_delegate) {
  unique_ptr<AidlInterface> interface;
  std::vector<std::unique_ptr<AidlImport>> imports;
  unique_ptr<cpp::TypeNamespace> types(new cpp::TypeNamespace());
  types->Init();
  AidlError err = internals::load_and_validate_aidl(
      std::vector<std::string>{},  // no preprocessed files
      options.ImportPaths(),
      options.InputFileName(),
      io_delegate,
      types.get(),
      &interface,
      &imports);
  if (err != AidlError::OK) {
    return 1;
  }

  string dep_file_name = options.DependencyFilePath();
  if (!dep_file_name.empty()) {
    generate_dep_file(dep_file_name, options.InputFileName(),
                      options.OutputCppFilePath(), imports, io_delegate);
  }

  return (cpp::GenerateCpp(options, *types, *interface, io_delegate)) ? 0 : 1;
}

int compile_aidl_to_java(const JavaOptions& options,
                         const IoDelegate& io_delegate) {
  unique_ptr<AidlInterface> interface;
  std::vector<std::unique_ptr<AidlImport>> imports;
  unique_ptr<java::JavaTypeNamespace> types(new java::JavaTypeNamespace());
  types->Init();
  AidlError aidl_err = internals::load_and_validate_aidl(
      options.preprocessed_files_,
      options.import_paths_,
      options.input_file_name_,
      io_delegate,
      types.get(),
      &interface,
      &imports);
  if (aidl_err == AidlError::FOUND_PARCELABLE && !options.fail_on_parcelable_) {
    // We aborted code generation because this file contains parcelables.
    // However, we were not told to complain if we find parcelables.
    // Just exit quietly.
    return 0;
  }
  if (aidl_err != AidlError::OK) {
    return 1;
  }

  string output_file_name = options.output_file_name_;
  // if needed, generate the output file name from the base folder
  if (output_file_name.empty() && !options.output_base_folder_.empty()) {
    output_file_name = generate_outputFileName(options, *interface);
  }

  // make sure the folders of the output file all exists
  if (!io_delegate.CreatePathForFile(output_file_name)) {
    return 1;
  }

  // if we were asked to, generate a make dependency file
  string dep_file_name = options.DependencyFilePath();
  if (!dep_file_name.empty()) {
    generate_dep_file(dep_file_name, options.input_file_name_,
                      options.output_file_name_, imports, io_delegate);
  }

  return generate_java(output_file_name, options.input_file_name_.c_str(),
                       interface.get(), types.get(), io_delegate);
}

int preprocess_aidl(const JavaOptions& options,
                    const IoDelegate& io_delegate) {
    vector<string> lines;

    // read files
    int N = options.files_to_preprocess_.size();
    for (int i=0; i<N; i++) {
        Parser p{io_delegate};
        if (!p.ParseFile(options.files_to_preprocess_[i]))
          return 1;
        AidlDocumentItem* doc = p.GetDocument();
        string line;
        if (doc->item_type == USER_DATA_TYPE) {
            AidlParcelable* parcelable = reinterpret_cast<AidlParcelable*>(doc);

            line = "parcelable ";

            if (! parcelable->GetPackage().empty()) {
                line += parcelable->GetPackage();
                line += '.';
            }
            line += parcelable->GetName();
        } else {
            line = "interface ";
            AidlInterface* iface = reinterpret_cast<AidlInterface*>(doc);
            if (!iface->GetPackage().empty()) {
                line += iface->GetPackage();
                line += '.';
            }
            line += iface->GetName();
        }
        line += ";\n";
        lines.push_back(line);
    }

    // write preprocessed file
    int fd = open( options.output_file_name_.c_str(),
                   O_RDWR|O_CREAT|O_TRUNC|O_BINARY,
#ifdef _WIN32
                   _S_IREAD|_S_IWRITE);
#else
                   S_IRUSR|S_IWUSR|S_IRGRP);
#endif
    if (fd == -1) {
        fprintf(stderr, "aidl: could not open file for write: %s\n",
                options.output_file_name_.c_str());
        return 1;
    }

    N = lines.size();
    for (int i=0; i<N; i++) {
        const string& s = lines[i];
        int len = s.length();
        if (len != write(fd, s.c_str(), len)) {
            fprintf(stderr, "aidl: error writing to file %s\n",
                options.output_file_name_.c_str());
            close(fd);
            unlink(options.output_file_name_.c_str());
            return 1;
        }
    }

    close(fd);
    return 0;
}

}  // namespace android
}  // namespace aidl
