#include "keyboard.h"
#include "uefi.h"
#include "apic.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02

static volatile UINT64 interrupt_count;
static volatile UINT8 last_scancode;
static volatile char character_queue[64];
static volatile UINT32 queue_read;
static volatile UINT32 queue_write;
static volatile UINT8 shift_pressed;

static char translate_scancode(UINT8 scancode)
{
    static const char normal[] = {
        0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[] = {
        0, 0, '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };

    if (scancode >= sizeof(normal)) {
        return 0;
    }
    return shift_pressed ? shifted[scancode] : normal[scancode];
}

static void queue_character(char character)
{
    UINT32 next = (queue_write + 1) % sizeof(character_queue);

    if (next != queue_read) {
        character_queue[queue_write] = character;
        queue_write = next;
    }
}

static int wait_input_empty(void)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 1000000U; timeout++) {
        if ((__inbyte(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return 1;
        }
    }

    return 0;
}

static void flush_output(void)
{
    UINT32 timeout;

    for (timeout = 0; timeout < 256U; timeout++) {
        if ((__inbyte(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0) {
            return;
        }
        (void)__inbyte(PS2_DATA_PORT);
    }
}

int keyboard_initialize(void)
{
    interrupt_count = 0;
    last_scancode = 0;
    queue_read = 0;
    queue_write = 0;
    shift_pressed = 0;

    /* Quick check: port 0x64 == 0xFF means no PS2 controller present
       (e.g. Hyper-V Gen2).  Avoid spinning 1 M VM-exits for nothing. */
    if (__inbyte(PS2_STATUS_PORT) == 0xFFU) {
        return 0;
    }

    if (!wait_input_empty()) {
        return 0;
    }

    __outbyte(PS2_COMMAND_PORT, 0xAD);
    flush_output();

    if (!wait_input_empty()) {
        return 0;
    }

    __outbyte(PS2_COMMAND_PORT, 0xAE);
    flush_output();
    return 1;
}

int keyboard_has_data(void)
{
    return (__inbyte(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0;
}

UINT8 keyboard_read_scancode(void)
{
    return __inbyte(PS2_DATA_PORT);
}

int keyboard_read_character(char *character)
{
    if (queue_read == queue_write) {
        return 0;
    }
    *character = character_queue[queue_read];
    queue_read = (queue_read + 1) % sizeof(character_queue);
    return 1;
}

void keyboard_poll_controller(void)
{
    UINT32 limit;

    for (limit = 0; limit < 16U && keyboard_has_data(); limit++) {
        keyboard_inject_scancode(keyboard_read_scancode());
    }
}

void keyboard_interrupt_handler(void)
{
    if (keyboard_has_data()) {
        char character;

        last_scancode = keyboard_read_scancode();
        interrupt_count++;
        if (last_scancode == 0x2A || last_scancode == 0x36) {
            shift_pressed = 1;
        } else if (last_scancode == 0xAA || last_scancode == 0xB6) {
            shift_pressed = 0;
        } else if ((last_scancode & 0x80) == 0) {
            character = translate_scancode(last_scancode);
            if (character != 0) {
                queue_character(character);
            }
        }
    }

    apic_eoi();
}

UINT64 keyboard_interrupt_count(void)
{
    return interrupt_count;
}

void keyboard_inject_character(char character)
{
    queue_character(character);
}

void keyboard_inject_scancode(UINT8 scancode)
{
    char character;

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return;
    }
    if ((scancode & 0x80) != 0) {
        return;
    }

    character = translate_scancode(scancode);
    if (character != 0) {
        queue_character(character);
    }
}
