#include "chibicc.h"

/*
# dyibicc obj v1\n

each record is
<4 byte record type and length><arbitrary data of length specified>
high byte is type, bottom 3 bytes are length

type 0: invalid

type 1: string (# variable)
padded to mod 4 boundary with \0

type 2: import (# 8)
<offset into code><string record index containing name>

type 3: function export (# 8)
<offset location in code><string record index containing name>

type 4: code reference to global (# 8)
<offset into code><string record index containing name>

type 5: initialized data (# 12)
<size><alignment><string record index containing name>
 4     4          4     
if <name> == 0, not exported
at least one type 6-9 must immediately follow type 5

type 6: end of initializers (# 0)
no further initializers for most recent data
any bytes of <size> that haven't been initialized are zeroed
(so bss will be type 5 followed by type 6)

type 7: bytes data initializer (# variable)
N db entries initializing most recent type 5

type 8: reloc data initializer to data (# 8)
<string record to refer to><addend>

type 9: reloc data initializer to code label (# 8)
<code offset><addend>

type 100: x64 code (# variable)
<array of x64 code bytes>
these are indexed by various types
there can only be one type 100 per file
it must be the last entry in the file

type 101: entry point (# 4)
<offset into code>


---



*/

// Note: this must be length mod 4 (\0 not included).
static char kSignature[] = "# dyibicc obj v1";

static int current_record_index = 0;
static HashMap string_to_record_index;

static bool write_int(FILE* f, unsigned int x) {
  return fwrite(&x, sizeof(x), 1, f) >= 0;
}

static bool write_record_header(FILE* f, unsigned int type, unsigned int length) {
  assert(length <= 0xffffff);
  assert(type > 0xffffff);
  unsigned int header = type | length;
  current_record_index++;
  return write_int(f, header);
}

static bool write_string2_uncached(FILE* f, char* str, int str_len, int* str_index) {
  int padding = (4 - str_len % 4) % 4;  // Pad to 4 byte boundary.

  unsigned int size = str_len + padding;

  if (!write_record_header(f, kTypeString, size))
    return false;

  if (fwrite(str, str_len, 1, f) < 0)
    return false;

  if (padding && fwrite("\0\0", padding, 1, f) < 0)
    return false;

  *str_index = current_record_index;
  return true;
}

static bool write_string_uncached(FILE* f, char* str, int* str_index) {
  return write_string2_uncached(f, str, strlen(str), str_index);
}

static bool write_string(FILE* f, char* str, int* str_index) {
  void* to = hashmap_get(&string_to_record_index, str);
  if (!to) {
    if (!write_string_uncached(f, str, str_index))
      return false;
    hashmap_put(&string_to_record_index, str, (void*)(uintptr_t)*str_index);
  } else {
    *str_index = (int)(uintptr_t)to;
  }

  return true;
}

bool write_dyo_begin(FILE* f) {
  current_record_index = 0;
  string_to_record_index = (HashMap){NULL, 0, 0};

  if (fwrite(kSignature, sizeof(kSignature) - 1, 1, f) < 0) {
    return false;
  }

  return true;
}

bool write_dyo_import(FILE* f, char* name, unsigned int loc) {
  int str_index;
  if (!write_string(f, name, &str_index))
    return false;

  if (!write_record_header(f, kTypeImport, 8))
    return false;
  if (!write_int(f, loc))
    return false;
  if (!write_int(f, str_index))
    return false;

  return true;
}

bool write_dyo_function_export(FILE* f, char* name, unsigned int loc) {
  int str_index;
  if (!write_string(f, name, &str_index))
    return false;

  if (!write_record_header(f, kTypeFunctionExport, 8))
    return false;
  if (!write_int(f, loc))
    return false;
  if (!write_int(f, str_index))
    return false;

  return true;
}

bool write_dyo_code_reference_to_global(FILE* f, char* name, unsigned int offset) {
  int str_index;
  if (!write_string(f, name, &str_index))
    return false;

  if (!write_record_header(f, kTypeCodeReferenceToGlobal, 8))
    return false;
  if (!write_int(f, offset))
    return false;
  if (!write_int(f, str_index))
    return false;

  return true;
}

bool write_dyo_initialized_data(FILE* f, int size, int align, int is_static, char* name) {
  int str_index = 0;
  if (name) {
    if (!write_string(f, name, &str_index))
      return false;
  }

  if (!write_record_header(f, kTypeInitializedData, 16))
    return false;
  if (!write_int(f, size))
    return false;
  if (!write_int(f, align))
    return false;
  if (!write_int(f, is_static))
    return false;
  if (!write_int(f, str_index))
    return false;

  return true;
}

bool write_dyo_initializer_end(FILE* f) {
  return write_record_header(f, kTypeInitializerEnd, 0);
}

bool write_dyo_initializer_bytes(FILE* f, char* data, int len) {
  if (!write_record_header(f, kTypeInitializerBytes, len))
    return false;

  if (fwrite(data, len, 1, f) < 0)
    return false;

  // XXX realign to 4

  return true;
}

bool write_dyo_initializer_data_relocation(FILE* f, char* name, int addend) {
  int str_index = 0;
  if (name) {
    if (!write_string(f, name, &str_index))
      return false;
  }

  if (!write_record_header(f, kTypeInitializerDataRelocation, 8))
    return false;
  if (!write_int(f, str_index))
    return false;
  if (!write_int(f, addend))
    return false;

  return true;
}

bool write_dyo_initializer_code_relocation(FILE* f, int pclabel, int addend, int* patch_loc) {
  if (!write_record_header(f, kTypeInitializerCodeRelocation, 8))
    return false;
  *patch_loc = ftell(f);
  if (!write_int(f, pclabel))
    return false;
  if (!write_int(f, addend))
    return false;

  return true;
}

bool patch_dyo_initializer_code_relocation(FILE* f, int file_loc, int final_code_offset) {
  long old = ftell(f);
  if (old < 0) {
    fprintf(stderr, "couldn't save old loc\n");
    return false;
  }

  if (fseek(f, file_loc, SEEK_SET) < 0) {
    fprintf(stderr, "couldn't seek to patch loc (%d)\n", file_loc);
    return false;
  }

  if (!write_int(f, final_code_offset)) {
    fprintf(stderr, "writing patch failed\n");
    return false;
  }

  if (fseek(f, old, SEEK_SET) < 0) {
    fprintf(stderr, "failed to restore loc\n");
    return false;
  }

  return true;
}

bool write_dyo_code(FILE* f, void* data, size_t size) {
  if (!write_record_header(f, kTypeX64Code, size))
    return false;

  if (fwrite(data, size, 1, f) < 0)
    return false;

  return true;
}

bool write_dyo_entrypoint(FILE* f, unsigned int loc) {
  if (!write_record_header(f, kTypeEntryPoint, 4))
    return false;
  if (!write_int(f, loc))
    return false;

  return true;
}

bool ensure_dyo_header(FILE* f) {
  char buf[sizeof(kSignature)];
  if (fread(buf, sizeof(kSignature) - 1, 1, f) < 0) {
    fprintf(stderr, "read error");
    return false;
  }
  if (strncmp(buf, kSignature, sizeof(kSignature) - 1) != 0) {
    fprintf(stderr, "signature doesn't match");
    return false;
  }
  return true;
}

bool read_dyo_record(FILE* f,
                     int* record_index,
                     char* buf,
                     unsigned int buf_size,
                     unsigned int* type,
                     unsigned int* size) {
  if (fread(buf, 4, 1, f) < 0) {
    fprintf(stderr, "read error");
    return false;
  }
  *record_index += 1;
  unsigned int header = *(unsigned int*)buf;
  *type = (header & 0xff000000);
  *size = (header & 0xffffff);
  if (*size > buf_size) {
    fprintf(stderr, "record larger than buffer (%d > %d)\n", *size, buf_size);
    return false;
  }
  if (fread(buf, *size, 1, f) < 0) {
    fprintf(stderr, "read error");
    return false;
  }
  
  return true;
}

bool dump_dyo_file(FILE* f) {
  char buf[1 << 20];  // Arbitrary max length.

  if (!ensure_dyo_header(f))
    return false;

  int record_index = 0;
  for (;;) {
    unsigned int type;
    unsigned int size;
    if (!read_dyo_record(f, &record_index, buf, sizeof(buf), &type, &size))
      return false;

    switch (type) {
      case kTypeString:
        printf("%4d string (%d bytes)\n", record_index, size);
        printf("        \"%.*s\"\n", size, buf);
        break;
      case kTypeImport:
        printf("%4d import (%d bytes)\n", record_index, size);
        printf("       fixup at %d\n", *(unsigned int*)&buf[0]);
        printf("       point at str record %d\n", *(unsigned int*)&buf[4]);
        break;
      case kTypeFunctionExport:
        printf("%4d function export (%d bytes)\n", record_index, size);
        printf("       function at %d\n", *(unsigned int*)&buf[0]);
        printf("       named by str record %d\n", *(unsigned int*)&buf[4]);
        break;
      case kTypeCodeReferenceToGlobal:
        printf("%4d code reference to global (%d bytes)\n", record_index, size);
        printf("       fixup at %d\n", *(unsigned int*)&buf[0]);
        printf("       point at str record %d\n", *(unsigned int*)&buf[4]);
        break;
      case kTypeInitializedData:
        printf("%4d initialized data (%d bytes)\n", record_index, size);
        printf("       size %d\n", *(unsigned int*)&buf[0]);
        printf("       align %d\n", *(unsigned int*)&buf[4]);
        printf("       is_static %d\n", *(unsigned int*)&buf[8]);
        printf("       name at str record %d\n", *(unsigned int*)&buf[12]);
        break;
      case kTypeInitializerEnd:
        printf("    ->%d initializers end (%d bytes)\n", record_index, size);
        break;
      case kTypeInitializerBytes:
        printf("    ->%d initializer bytes (%d bytes)\n", record_index, size);
        printf("         ");
        for (int i = 0; i < size; ++i) {
          printf(" 0x%x", buf[i]);
        }
        printf("\n");
        break;
      case kTypeInitializerDataRelocation:
        printf("    ->%d initializer data relocation (%d bytes)\n", record_index, size);
        printf("        name at str record %d\n", *(unsigned int*)&buf[0]);
        printf("        addend %d\n", *(int*)&buf[4]);
        break;
      case kTypeInitializerCodeRelocation:
        printf("    ->%d initializer code relocation (%d bytes)\n", record_index, size);
        printf("        pclabel %d\n", *(unsigned int*)&buf[0]);
        printf("        addend %d\n", *(int*)&buf[4]);
        break;
      case kTypeX64Code:
        printf("%4d code (%d bytes)\n", record_index, size);
        printf("--------------------\n");
        fflush(stdout);
        FILE* tmp = fopen("tmp.raw", "wb");
        if (!tmp) {
          fprintf(stderr, "failed to write tmp file for ndisasm\n");
          return false;
        }
        fwrite(buf, size, 1, tmp);
        fclose(tmp);
        if (system("ndisasm -b64 tmp.raw") < 0) {
          fprintf(stderr, "failed to run ndisasm\n");
          return false;
        }
        printf("--------------------\n");
        return true;
      case kTypeEntryPoint:
        printf("%4d entry point (%d bytes)\n", record_index, size);
        printf("       located at offset %d\n", *(unsigned int*)&buf[0]);
        break;
      default:
        printf("unhandled record type %x (%d bytes)\n", type, size);
        break;
    }
  }
}