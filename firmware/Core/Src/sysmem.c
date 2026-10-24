#include <errno.h>
#include <stdint.h>
#include <stddef.h>

/* Heap for newlib (vsnprintf can ask for a little). Grows up from the end
 * of .bss and stops short of the reserved stack. */
void *_sbrk(ptrdiff_t incr)
{
  extern uint8_t _end;
  extern uint8_t _estack;
  extern uint32_t _Min_Stack_Size;
  const uint32_t stack_limit = (uint32_t)&_estack - (uint32_t)&_Min_Stack_Size;
  static uint8_t *heap_end = NULL;

  if (heap_end == NULL) heap_end = &_end;
  if ((uint32_t)heap_end + incr > stack_limit)
  {
    errno = ENOMEM;
    return (void *)-1;
  }
  uint8_t *prev = heap_end;
  heap_end += incr;
  return prev;
}
