#include "face_db.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

namespace {
struct old_face_entry_t {
    char name[FACE_DB_NAME_LEN];
    float embedding[FACE_DB_EMBED_DIM];
};

struct metadata_v1_face_entry_t {
    char name[FACE_DB_NAME_LEN];
    char sex[FACE_DB_SEX_LEN];
    char cccd[FACE_DB_CCCD_LEN];
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
                db->entries[i].sex[0] = '\0';
                db->entries[i].cccd[0] = '\0';
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
                copy_string(db->entries[i].sex, FACE_DB_SEX_LEN,
                            old_entry.sex);
                copy_string(db->entries[i].cccd, FACE_DB_CCCD_LEN,
                            old_entry.cccd);
                db->entries[i].employee_id[0] = '\0';
                db->entries[i].company_id[0] = '\0';
                memcpy(db->entries[i].embedding, old_entry.embedding,
                       FACE_DB_EMBED_DIM * sizeof(float));
            }
        } else {
            db->count = 0;
            fclose(f);
            return -1;
        }
    }

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
    return face_db_add_with_info(db, name, "", "", "", "", embedding);
}

int face_db_add_with_info(face_db_t *db,
                          const char *name,
                          const char *sex,
                          const char *cccd,
                          const char *employee_id,
                          const char *company_id,
                          const float *embedding)
{
    if (db->count >= FACE_DB_MAX_ENTRIES) {
        printf("[face_db] Database full (%d entries)\n", FACE_DB_MAX_ENTRIES);
        return -1;
    }

    face_entry_t *e = &db->entries[db->count];
    copy_string(e->name, FACE_DB_NAME_LEN, name);
    copy_string(e->sex, FACE_DB_SEX_LEN, sex);
    copy_string(e->cccd, FACE_DB_CCCD_LEN, cccd);
    copy_string(e->employee_id, FACE_DB_EMPLOYEE_ID_LEN, employee_id);
    copy_string(e->company_id, FACE_DB_COMPANY_ID_LEN, company_id);
    memcpy(e->embedding, embedding, FACE_DB_EMBED_DIM * sizeof(float));

    db->count++;
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
        if (db->entries[i].cccd[0] != '\0' || db->entries[i].sex[0] != '\0' ||
            db->entries[i].employee_id[0] != '\0' ||
            db->entries[i].company_id[0] != '\0') {
            printf("  [%d] %s sex=%s cccd=%s employee_id=%s company_id=%s\n",
                   i, db->entries[i].name, db->entries[i].sex,
                   db->entries[i].cccd, db->entries[i].employee_id,
                   db->entries[i].company_id);
        } else {
            printf("  [%d] %s\n", i, db->entries[i].name);
        }
    }
}
