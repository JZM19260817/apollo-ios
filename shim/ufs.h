#pragma once
void test_ufs(void);
int ufs_write_log(const void *buf, unsigned len);
int ufs_read(uint32_t lba, uint32_t count, void *buf);
int ufs_find(const char *name, uint32_t *lba, uint32_t *nblk);
int ufs_init(void);
int ufs_read_raw(uint32_t lba, uint32_t count, void *buf);
