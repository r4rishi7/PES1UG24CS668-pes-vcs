// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration — object_write is implemented in object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Comparison function for qsort
static int entry_cmp(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// Load the index from .pes/index.
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    // Only set count to 0 — do NOT memset the entire 5.6 MB array.
    // The entries array lives on the stack (declared in pes.c cmd_add).
    // Touching all 5.6 MB with memset forces the OS to allocate every
    // stack page, which overflows the default 8 MB stack limit.
    index->count = 0;

    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        // File doesn't exist = empty index = perfectly normal
        return 0;
    }

    while (index->count < MAX_INDEX_ENTRIES) {
        unsigned int mode;
        char hex_hash[HASH_HEX_SIZE + 1];
        unsigned long mtime;
        unsigned int size;
        char path[512];

        int parsed = fscanf(fp, "%o %64s %lu %u %511s",
                            &mode, hex_hash, &mtime, &size, path);

        if (parsed == EOF) {
            break;
        }
        if (parsed != 5) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF);
            continue;
        }

        IndexEntry *entry = &index->entries[index->count];
        memset(entry, 0, sizeof(IndexEntry));  // Zero only THIS one entry (560 bytes)
        entry->mode = (uint32_t)mode;
        hex_to_hash(hex_hash, &entry->hash);
        entry->mtime_sec = (uint64_t)mtime;
        entry->size = (uint32_t)size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';

        index->count++;
    }

    fclose(fp);
    return 0;
}

// Save the index to .pes/index atomically.
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Sort only the active entries using an index array on the heap.
    // Do NOT copy the full Index struct — it's 5.6 MB and will overflow the stack.

    int *order = NULL;
    if (index->count > 0) {
        order = malloc(index->count * sizeof(int));
        if (!order) {
            perror("index_save: malloc");
            return -1;
        }
        for (int i = 0; i < index->count; i++) {
            order[i] = i;
        }

        // Simple insertion sort on the index array (by path)
        for (int i = 1; i < index->count; i++) {
            int key = order[i];
            int j = i - 1;
            while (j >= 0 && strcmp(index->entries[order[j]].path,
                                    index->entries[key].path) > 0) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = key;
        }
    }

    // Create temp file for atomic write
    char tmp_path[] = PES_DIR "/index.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        perror("index_save: mkstemp");
        free(order);
        return -1;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        perror("index_save: fdopen");
        close(fd);
        unlink(tmp_path);
        free(order);
        return -1;
    }

    // Write each entry as one text line
    for (int i = 0; i < index->count; i++) {
        const IndexEntry *entry = &index->entries[order[i]];
        char hex_hash[HASH_HEX_SIZE + 1];
        hash_to_hex(&entry->hash, hex_hash);

        fprintf(fp, "%06o %s %lu %u %s\n",
                entry->mode,
                hex_hash,
                (unsigned long)entry->mtime_sec,
                (unsigned int)entry->size,
                entry->path);
    }

    // Flush → sync → close → rename
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(order);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        perror("index_save: rename");
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Step 1: Stat the file
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s': ", path);
        perror("");
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    // Step 2: Read file contents
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s': ", path);
        perror("");
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    uint8_t *content = malloc(file_size > 0 ? file_size : 1);
    if (!content) {
        fprintf(stderr, "error: out of memory\n");
        fclose(fp);
        return -1;
    }

    if (file_size > 0) {
        size_t bytes_read = fread(content, 1, file_size, fp);
        if (bytes_read != file_size) {
            fprintf(stderr, "error: short read on '%s'\n", path);
            free(content);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);

    // Step 3: Write blob to object store
    ObjectID blob_hash;
    if (object_write(OBJ_BLOB, content, file_size, &blob_hash) != 0) {
        fprintf(stderr, "error: failed to write blob for '%s'\n", path);
        free(content);
        return -1;
    }
    free(content);

    // Step 4: Determine file mode
    uint32_t mode;
    if (st.st_mode & S_IXUSR) {
        mode = 0100755;
    } else {
        mode = 0100644;
    }

    // Step 5: Update or create index entry
    IndexEntry *existing = index_find(index, path);

    if (existing) {
        existing->hash = blob_hash;
        existing->mode = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full (max %d entries)\n", MAX_INDEX_ENTRIES);
            return -1;
        }

        IndexEntry *new_entry = &index->entries[index->count];
        memset(new_entry, 0, sizeof(IndexEntry));  // Zero only this entry
        new_entry->mode = mode;
        new_entry->hash = blob_hash;
        new_entry->mtime_sec = (uint64_t)st.st_mtime;
        new_entry->size = (uint32_t)st.st_size;
        strncpy(new_entry->path, path, sizeof(new_entry->path) - 1);
        new_entry->path[sizeof(new_entry->path) - 1] = '\0';

        index->count++;
    }

    // Step 6: Save atomically
    return index_save(index);
}
