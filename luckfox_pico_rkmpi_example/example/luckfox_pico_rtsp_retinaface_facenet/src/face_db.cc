#include "face_db.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

namespace {
constexpr size_t kLegacySexLen = 16;
constexpr size_t kLegacyCccdLen = 32;
constexpr size_t kLegacyCompanyIdLen = 64;

struct old_face_entry_t {
    char name[FACE_DB_NAME_LEN];
    float embedding[FACE_DB_EMBED_DIM];
};

struct metadata_v1_face_entry_t {
    char name[FACE_DB_NAME_LEN];
    char sex[kLegacySexLen];
    char cccd[kLegacyCccdLen];
    float embedding[FACE_DB_EMBED_DIM];
};

// Layout used before audio_link replaced sex/cccd/company_id. Keep this only
// for a one-way, in-memory migration of deployed face_db.bin files.
struct metadata_v2_face_entry_t {
    char name[FACE_DB_NAME_LEN];
    char sex[kLegacySexLen];
    char cccd[kLegacyCccdLen];
    char employee_id[FACE_DB_EMPLOYEE_ID_LEN];
    char company_id[kLegacyCompanyIdLen];
    float embedding[FACE_DB_EMBED_DIM];
};

void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

bool employee_id_equal(const char *left, const char *right)
{
    if (!left || !right || !left[0] || !right[0])
        return false;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return *left == '\0' && *right == '\0';
}

void merge_duplicate_employees(face_db_t *db)
{
    for (int i = 0; i < db->count; ++i) {
        int j = i + 1;
        while (j < db->count) {
            if (!employee_id_equal(db->entries[i].employee_id,
                                   db->entries[j].employee_id)) {
                ++j;
                continue;
            }

            // The later record is the most recent registration. Preserve an
            // older audio path only when the newer record has none.
            face_entry_t merged = db->entries[j];
            if (merged.audio_path[0] == '\0' &&
                db->entries[i].audio_path[0] != '\0') {
                copy_string(merged.audio_path, FACE_DB_AUDIO_PATH_LEN,
                            db->entries[i].audio_path);
            }
            db->entries[i] = merged;
            if (j + 1 < db->count) {
                memmove(&db->entries[j], &db->entries[j + 1],
                        (size_t)(db->count - j - 1) * sizeof(face_entry_t));
            }
            memset(&db->entries[db->count - 1], 0, sizeof(face_entry_t));
            --db->count;
            printf("[face_db] Merged duplicate employee_id=%s\n",
                   db->entries[i].employee_id);
        }
    }
}
}  // namespace

int face_db_load(face_db_t *db, const char *path)
{
    memset(db, 0, sizeof(face_db_t));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;   /* not found – caller treats as empty DB */

    if (fread(&db->count, sizeof(int), 1, f) != 1) {
        fclose(f);
        db->count = 0;
        return -1;
    }
    if (db->count < 0 || db->count > FACE_DB_MAX_ENTRIES)
        db->count = 0;

    size_t n = (size_t)db->count;
    if (n > 0) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        bool has_size = stat(path, &st) == 0;
        size_t payload_size = has_size && st.st_size >= (off_t)sizeof(int)
                                  ? (size_t)st.st_size - sizeof(int)
                                  : 0;

        if (payload_size == n * sizeof(face_entry_t)) {
            if (fread(db->entries, sizeof(face_entry_t), n, f) != n) {
                db->count = 0;
                fclose(f);
                return -1;
            }
        } else if (payload_size == n * sizeof(old_face_entry_t)) {
            for (size_t i = 0; i < n; ++i) {
                old_face_entry_t old_entry;
                if (fread(&old_entry, sizeof(old_face_entry_t), 1, f) != 1) {
                    db->count = 0;
                    fclose(f);
                    return -1;
                }
                copy_string(db->entries[i].name, FACE_DB_NAME_LEN,
                            old_entry.name);
                memcpy(db->entries[i].embedding, old_entry.embedding,
                       FACE_DB_EMBED_DIM * sizeof(float));
            }
        } else if (payload_size == n * sizeof(metadata_v1_face_entry_t)) {
            for (size_t i = 0; i < n; ++i) {
                metadata_v1_face_entry_t old_entry;
                if (fread(&old_entry, sizeof(old_entry), 1, f) != 1) {
                    db->count = 0;
                    fclose(f);
                    return -1;
                }
                copy_string(db->entries[i].name, FACE_DB_NAME_LEN,
                            old_entry.name);
                memcpy(db->entries[i].embedding, old_entry.embedding,
                       FACE_DB_EMBED_DIM * sizeof(float));
            }
        } else if (payload_size == n * sizeof(metadata_v2_face_entry_t)) {
            for (size_t i = 0; i < n; ++i) {
                metadata_v2_face_entry_t old_entry;
                if (fread(&old_entry, sizeof(old_entry), 1, f) != 1) {
                    db->count = 0;
                    fclose(f);
                    return -1;
                }
                copy_string(db->entries[i].name, FACE_DB_NAME_LEN,
                            old_entry.name);
                copy_string(db->entries[i].employee_id,
                            FACE_DB_EMPLOYEE_ID_LEN,
                            old_entry.employee_id);
                memcpy(db->entries[i].embedding, old_entry.embedding,
                       FACE_DB_EMBED_DIM * sizeof(float));
            }
        } else {
            db->count = 0;
            fclose(f);
            return -1;
        }
    }

    merge_duplicate_employees(db);
    fclose(f);
    return 0;
}

int face_db_save(const face_db_t *db, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[face_db] Cannot open %s for writing\n", path);
        return -1;
    }

    fwrite(&db->count, sizeof(int), 1, f);
    if (db->count > 0)
        fwrite(db->entries, sizeof(face_entry_t), (size_t)db->count, f);

    fclose(f);
    return 0;
}

int face_db_add(face_db_t *db, const char *name, const float *embedding)
{
    return face_db_add_with_info(db, name, "", "", embedding);
}

int face_db_add_with_info(face_db_t *db,
                          const char *name,
                          const char *employee_id,
                          const char *audio_path,
                          const float *embedding)
{
    int index = -1;
    if (employee_id && employee_id[0] != '\0') {
        for (int i = 0; i < db->count; ++i) {
            if (employee_id_equal(db->entries[i].employee_id, employee_id)) {
                index = i;
                break;
            }
        }
    }

    if (index < 0 && db->count >= FACE_DB_MAX_ENTRIES) {
        printf("[face_db] Database full (%d entries)\n", FACE_DB_MAX_ENTRIES);
        return -1;
    }
    const bool updating = index >= 0;
    if (!updating)
        index = db->count;

    face_entry_t *e = &db->entries[index];
    copy_string(e->name, FACE_DB_NAME_LEN, name);
    copy_string(e->employee_id, FACE_DB_EMPLOYEE_ID_LEN, employee_id);
    copy_string(e->audio_path, FACE_DB_AUDIO_PATH_LEN, audio_path);
    memcpy(e->embedding, embedding, FACE_DB_EMBED_DIM * sizeof(float));

    if (updating) {
        printf("[face_db] Updated employee_id=%s at index %d\n",
               e->employee_id, index);
    } else {
        db->count++;
    }
    return 0;
}

int face_db_find(const face_db_t *db,
                 const float     *embedding,
                 float           *out_dist)
{
    if (db->count == 0) {
        *out_dist = 9999.0f;
        return -1;
    }

    int   best_idx  = 0;
    float best_dist = 9999.0f;

    for (int i = 0; i < db->count; i++) {
        float sum = 0.0f;
        for (int j = 0; j < FACE_DB_EMBED_DIM; j++) {
            float d = embedding[j] - db->entries[i].embedding[j];
            sum += d * d;
        }
        float dist = sqrtf(sum);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = i;
        }
    }

    *out_dist = best_dist;
    return best_idx;
}

void face_db_print(const face_db_t *db)
{
    printf("[face_db] %d registered face(s):\n", db->count);
    for (int i = 0; i < db->count; i++) {
        if (db->entries[i].employee_id[0] != '\0' ||
            db->entries[i].audio_path[0] != '\0') {
            printf("  [%d] %s employee_id=%s audio=%s\n",
                   i, db->entries[i].name, db->entries[i].employee_id,
                   db->entries[i].audio_path);
        } else {
            printf("  [%d] %s\n", i, db->entries[i].name);
        }
    }
}
