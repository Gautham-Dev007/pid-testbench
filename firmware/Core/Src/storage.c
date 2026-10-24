#include "storage.h"

#include <string.h>

#include "main.h"

#define PAGE_INDEX   (FLASH_PAGE_NB - 1u)
#define PAGE_ADDR    (FLASH_BASE + PAGE_INDEX * FLASH_PAGE_SIZE)
#define MAGIC        0x54444950u  /* "PIDT" */
#define VERSION      1u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  Settings s;
  uint32_t crc;
} Record;

/* Flash is programmed 64 bits at a time, so slots are rounded up to 8 bytes. */
#define SLOT_SIZE  ((sizeof(Record) + 7u) & ~7u)
#define SLOT_COUNT (FLASH_PAGE_SIZE / SLOT_SIZE)

static uint32_t crc32(const void *data, size_t len)
{
  const uint8_t *p   = data;
  uint32_t       crc = 0xFFFFFFFFu;
  while (len--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static const Record *slot(uint32_t i) { return (const Record *)(PAGE_ADDR + i * SLOT_SIZE); }

static bool slot_empty(uint32_t i)
{
  const uint32_t *w = (const uint32_t *)slot(i);
  return w[0] == 0xFFFFFFFFu && w[1] == 0xFFFFFFFFu;
}

static bool slot_valid(uint32_t i)
{
  const Record *r = slot(i);
  return r->magic == MAGIC && r->version == VERSION && r->size == sizeof(Settings) &&
         r->crc == crc32(&r->s, sizeof r->s);
}

void storage_defaults(Settings *s)
{
  memset(s, 0, sizeof *s);
  s->setpoint = SETPOINT_DEFAULT;
  s->volume   = BUZZER_DEFAULT_VOLUME;
  s->zn       = (Gains){MANUAL_KP, MANUAL_KI, MANUAL_KD};
}

bool storage_load(Settings *s)
{
  int last = -1;
  for (uint32_t i = 0; i < SLOT_COUNT; i++) {
    if (slot_empty(i)) break;
    if (slot_valid(i)) last = (int)i;
  }
  if (last < 0) {
    storage_defaults(s);
    return false;
  }
  memcpy(s, &slot((uint32_t)last)->s, sizeof *s);
  return true;
}

bool storage_erase(void)
{
  FLASH_EraseInitTypeDef erase = {
    .TypeErase = FLASH_TYPEERASE_PAGES,
    .Banks     = FLASH_BANK_1,
    .Page      = PAGE_INDEX,
    .NbPages   = 1,
  };
  uint32_t bad_page = 0;

  HAL_FLASH_Unlock();
  FLASH->SR = FLASH_SR_CLEAR;  /* drop stale error flags from earlier writes */
  bool ok = HAL_FLASHEx_Erase(&erase, &bad_page) == HAL_OK;
  HAL_FLASH_Lock();
  return ok;
}

bool storage_save(const Settings *s)
{
  uint8_t buf[SLOT_SIZE];
  memset(buf, 0xFF, sizeof buf);
  Record *r  = (Record *)buf;
  r->magic   = MAGIC;
  r->version = VERSION;
  r->size    = sizeof(Settings);
  memcpy(&r->s, s, sizeof *s);
  r->crc     = crc32(&r->s, sizeof r->s);

  uint32_t next = 0;
  int      last = -1;
  while (next < SLOT_COUNT && !slot_empty(next)) {
    if (slot_valid(next)) last = (int)next;
    next++;
  }
  if (last >= 0 && memcmp(&slot((uint32_t)last)->s, s, sizeof *s) == 0) return true;

  if (next >= SLOT_COUNT) {
    if (!storage_erase()) return false;
    next = 0;
  }

  uint32_t addr = PAGE_ADDR + next * SLOT_SIZE;
  bool     ok   = true;

  HAL_FLASH_Unlock();
  FLASH->SR = FLASH_SR_CLEAR;  /* drop stale error flags from earlier writes */
  for (uint32_t off = 0; off < SLOT_SIZE && ok; off += 8u) {
    uint64_t dw;
    memcpy(&dw, buf + off, 8);
    ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + off, dw) == HAL_OK;
  }
  HAL_FLASH_Lock();

  return ok && slot_valid(next);
}
