/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure HID Report-Map parser (milestone M2).
 *
 * Walks the HID report-descriptor item stream (USB HID 1.11 §6.2.2) tracking
 * the global / local item state and records the bit positions of the
 * Buttons / X / Y / Wheel / AC-Pan fields of the FIRST usable pointer (mouse)
 * collection. No Zephyr dependencies -- host-unit-testable (see tests/parser/).
 */

#include <zmk_ble_hid_host/hid_report_parser.h>

#include <string.h>

/* ── HID item prefix decode (short items; 0xFE marks a long item) ─────────── */
#define HID_ITEM_LONG 0xFE

#define HID_TYPE_MAIN   0
#define HID_TYPE_GLOBAL 1
#define HID_TYPE_LOCAL  2

/* Main item tags (bType == MAIN). */
#define HID_MAIN_INPUT          0x8
#define HID_MAIN_OUTPUT         0x9
#define HID_MAIN_FEATURE        0xB
#define HID_MAIN_COLLECTION     0xA
#define HID_MAIN_END_COLLECTION 0xC

/* Global item tags (bType == GLOBAL). */
#define HID_GLOBAL_USAGE_PAGE   0x0
#define HID_GLOBAL_LOGICAL_MIN  0x1
#define HID_GLOBAL_LOGICAL_MAX  0x2
#define HID_GLOBAL_REPORT_SIZE  0x7
#define HID_GLOBAL_REPORT_ID    0x8
#define HID_GLOBAL_REPORT_COUNT 0x9
#define HID_GLOBAL_PUSH         0xA
#define HID_GLOBAL_POP          0xB

/* Local item tags (bType == LOCAL). */
#define HID_LOCAL_USAGE     0x0
#define HID_LOCAL_USAGE_MIN 0x1
#define HID_LOCAL_USAGE_MAX 0x2

/* Main "Input/Output/Feature" data-byte flags. */
#define HID_IO_CONSTANT 0x01 /* 0 = Data, 1 = Constant (padding)         */
#define HID_IO_VARIABLE 0x02 /* 0 = Array, 1 = Variable (one usage/field) */

/* Collection types. */
#define HID_COLLECTION_PHYSICAL    0x00
#define HID_COLLECTION_APPLICATION 0x01

/* Usage pages we care about. */
#define PAGE_GENERIC_DESKTOP 0x01
#define PAGE_BUTTON          0x09
#define PAGE_CONSUMER        0x0C

/* Generic Desktop usages. */
#define USAGE_GD_POINTER 0x01
#define USAGE_GD_MOUSE   0x02
#define USAGE_GD_X       0x30
#define USAGE_GD_Y       0x31
#define USAGE_GD_WHEEL   0x38

/* Consumer usage. */
#define USAGE_CONSUMER_AC_PAN 0x0238

#define MAX_LOCAL_USAGES 16
#define GLOBAL_STACK_DEPTH 8

/* A 32-bit "extended" usage: page in the high 16 bits, usage id in the low. */
#define USAGE_PAGE(u) ((uint16_t)((u) >> 16))
#define USAGE_ID(u)   ((uint16_t)((u) & 0xFFFF))
#define MAKE_USAGE(page, id) (((uint32_t)(page) << 16) | (uint16_t)(id))

/* Global item state saved/restored by Push/Pop. */
struct global_state {
    uint16_t usage_page;
    int32_t logical_min; /* sign-extended; <0 means the field is signed */
    uint8_t report_size;
    uint8_t report_count;
    uint8_t report_id;
};

struct parser {
    struct global_state g;
    struct global_state stack[GLOBAL_STACK_DEPTH];
    size_t stack_depth;

    /* Local item state -- cleared after every Main item. */
    uint32_t usages[MAX_LOCAL_USAGES];
    size_t usage_count;
    uint32_t usage_min;
    uint32_t usage_max;
    bool has_usage_range;

    /* Where the running Input-report bit cursor is, for the current report id.
     * 32-bit so a pathological descriptor can't wrap it; offsets that don't fit
     * the 16-bit layout fields are simply not recorded (see set_field). */
    uint32_t bit_cursor;

    /* Mouse application-collection tracking. */
    int depth;             /* current collection nesting depth                 */
    int mouse_level;       /* depth at which the mouse app opened (-1 none,     */
                           /*  -2 == found and now closed: stop assigning)      */

    struct zmk_hid_report_layout *out;
};

/* Read `size` raw bytes little-endian into an unsigned value. */
static uint32_t item_u(const uint8_t *d, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) {
        v |= (uint32_t)d[i] << (8 * i);
    }
    return v;
}

/* Read `size` bytes little-endian as a sign-extended value. */
static int32_t item_s(const uint8_t *d, uint8_t size) {
    uint32_t v = item_u(d, size);
    if (size > 0 && size < 4) {
        uint32_t sign_bit = 1u << (8 * size - 1);
        if (v & sign_bit) {
            v |= ~((1u << (8 * size)) - 1); /* extend the sign upward */
        }
    }
    return (int32_t)v;
}

static bool in_mouse(const struct parser *p) {
    return p->mouse_level >= 0;
}

static void clear_local(struct parser *p) {
    p->usage_count = 0;
    p->usage_min = 0;
    p->usage_max = 0;
    p->has_usage_range = false;
}

/* The extended usage that applies to slot `i` of a Variable Input item:
 * an explicit per-field usage list takes precedence; otherwise a Usage
 * Min..Max range maps slot i -> min+i; the last value repeats past the end. */
static uint32_t usage_for_slot(const struct parser *p, size_t i) {
    if (p->usage_count > 0) {
        size_t idx = (i < p->usage_count) ? i : (p->usage_count - 1);
        return p->usages[idx];
    }
    if (p->has_usage_range) {
        uint32_t u = p->usage_min + (uint32_t)i;
        if (u > p->usage_max) {
            u = p->usage_max;
        }
        return MAKE_USAGE(p->g.usage_page, u);
    }
    return MAKE_USAGE(p->g.usage_page, 0);
}

#define BIT_OFFSET_MAX 0xFFFFu /* layout bit_offset is uint16_t */

static void set_field(struct zmk_hid_field *f, uint32_t offset, uint8_t size, bool is_signed) {
    if (f->bit_size != 0) {
        return; /* keep the first occurrence */
    }
    if (offset + size > BIT_OFFSET_MAX) {
        return; /* offset doesn't fit the 16-bit layout -- ignore (no wraparound) */
    }
    f->bit_offset = (uint16_t)offset;
    f->bit_size = size;
    f->is_signed = is_signed;
}

/* Record the pointer fields carried by one Variable, Data Input main item. */
static void assign_input(struct parser *p, uint32_t start_bit) {
    struct zmk_hid_report_layout *o = p->out;
    bool is_signed = p->g.logical_min < 0;

    if (o->report_id == 0 && o->buttons.bit_size == 0 && !o->x.bit_size && !o->y.bit_size) {
        o->report_id = p->g.report_id; /* lock to the report this pointer lives in */
    }

    /* A Button-page item is one contiguous button bitfield. */
    if (p->g.usage_page == PAGE_BUTTON) {
        if (o->buttons.bit_size == 0) {
            uint8_t count = p->g.report_count;
            if (count > ZMK_HID_MAX_BUTTONS) {
                count = ZMK_HID_MAX_BUTTONS;
            }
            uint8_t size = p->g.report_size ? p->g.report_size : 1;
            /* Whole button run must fit the 16-bit layout offset. */
            if (start_bit + (uint32_t)count * size <= BIT_OFFSET_MAX) {
                o->buttons.bit_offset = (uint16_t)start_bit;
                o->buttons.bit_size = size; /* per-button width (1 for a normal mouse) */
                o->buttons.is_signed = false;
                o->button_count = count;
            }
        }
        return;
    }

    /* Otherwise each of report_count slots carries its own usage. */
    for (size_t i = 0; i < p->g.report_count; i++) {
        uint32_t u = usage_for_slot(p, i);
        uint32_t off = start_bit + (uint32_t)i * p->g.report_size;
        uint16_t page = USAGE_PAGE(u);
        uint16_t id = USAGE_ID(u);

        if (page == PAGE_GENERIC_DESKTOP && id == USAGE_GD_X) {
            set_field(&o->x, off, p->g.report_size, is_signed);
        } else if (page == PAGE_GENERIC_DESKTOP && id == USAGE_GD_Y) {
            set_field(&o->y, off, p->g.report_size, is_signed);
        } else if (page == PAGE_GENERIC_DESKTOP && id == USAGE_GD_WHEEL) {
            set_field(&o->wheel, off, p->g.report_size, is_signed);
        } else if (page == PAGE_CONSUMER && id == USAGE_CONSUMER_AC_PAN) {
            set_field(&o->hwheel, off, p->g.report_size, is_signed);
        }
    }
}

static void handle_main(struct parser *p, uint8_t tag, const uint8_t *data, uint8_t size) {
    switch (tag) {
    case HID_MAIN_INPUT: {
        uint32_t flags = item_u(data, size);
        uint32_t total_bits = (uint32_t)p->g.report_size * p->g.report_count;

        if (in_mouse(p) && !(flags & HID_IO_CONSTANT) && (flags & HID_IO_VARIABLE)) {
            assign_input(p, p->bit_cursor);
        }
        /* Constant padding still occupies bits. Saturate rather than wrap so a
         * pathological descriptor can't fold the cursor back over real fields. */
        if (p->bit_cursor + total_bits < p->bit_cursor) {
            p->bit_cursor = UINT32_MAX;
        } else {
            p->bit_cursor += total_bits;
        }
        break;
    }
    case HID_MAIN_OUTPUT:
    case HID_MAIN_FEATURE:
        /* Separate report streams -- they never consume Input-report bits. */
        break;
    case HID_MAIN_COLLECTION: {
        uint32_t type = item_u(data, size);
        bool is_mouse_usage = false;

        if (p->usage_count > 0) {
            uint32_t u = p->usages[p->usage_count - 1];
            is_mouse_usage = (USAGE_PAGE(u) == PAGE_GENERIC_DESKTOP && USAGE_ID(u) == USAGE_GD_MOUSE);
        }
        if (type == HID_COLLECTION_APPLICATION && is_mouse_usage && p->mouse_level == -1) {
            p->mouse_level = p->depth; /* this collection opens the mouse app */
        }
        p->depth++;
        break;
    }
    case HID_MAIN_END_COLLECTION:
        if (p->depth > 0) {
            p->depth--;
        }
        if (p->mouse_level >= 0 && p->depth <= p->mouse_level) {
            p->mouse_level = -2; /* mouse app closed: lock the layout */
        }
        break;
    default:
        break;
    }

    clear_local(p); /* local state resets after every Main item */
}

static void handle_global(struct parser *p, uint8_t tag, const uint8_t *data, uint8_t size) {
    switch (tag) {
    case HID_GLOBAL_USAGE_PAGE:
        p->g.usage_page = (uint16_t)item_u(data, size);
        break;
    case HID_GLOBAL_LOGICAL_MIN:
        p->g.logical_min = item_s(data, size);
        break;
    case HID_GLOBAL_LOGICAL_MAX:
        /* not needed for decoding; sign comes from logical_min */
        break;
    case HID_GLOBAL_REPORT_SIZE:
        p->g.report_size = (uint8_t)item_u(data, size);
        break;
    case HID_GLOBAL_REPORT_COUNT:
        p->g.report_count = (uint8_t)item_u(data, size);
        break;
    case HID_GLOBAL_REPORT_ID: {
        uint8_t id = (uint8_t)item_u(data, size);
        if (id != p->g.report_id) {
            p->bit_cursor = 0; /* each report id has its own bit stream */
        }
        p->g.report_id = id;
        break;
    }
    case HID_GLOBAL_PUSH:
        if (p->stack_depth < GLOBAL_STACK_DEPTH) {
            p->stack[p->stack_depth++] = p->g;
        }
        break;
    case HID_GLOBAL_POP:
        if (p->stack_depth > 0) {
            p->g = p->stack[--p->stack_depth];
        }
        break;
    default:
        break;
    }
}

static void handle_local(struct parser *p, uint8_t tag, const uint8_t *data, uint8_t size) {
    switch (tag) {
    case HID_LOCAL_USAGE: {
        /* A 4-byte usage embeds its own page in the high word; a 1/2-byte usage
         * is on the current global usage page. */
        uint32_t u = (size == 4) ? item_u(data, size)
                                 : MAKE_USAGE(p->g.usage_page, (uint16_t)item_u(data, size));
        if (p->usage_count < MAX_LOCAL_USAGES) {
            p->usages[p->usage_count++] = u;
        }
        break;
    }
    case HID_LOCAL_USAGE_MIN:
        p->usage_min = item_u(data, size);
        p->has_usage_range = true;
        break;
    case HID_LOCAL_USAGE_MAX:
        p->usage_max = item_u(data, size);
        p->has_usage_range = true;
        break;
    default:
        break;
    }
}

int zmk_hid_parse_report_map(const uint8_t *report_map, size_t len,
                             struct zmk_hid_report_layout *out) {
    if (!report_map || !out) {
        return -1;
    }

    struct parser p;
    memset(&p, 0, sizeof(p));
    memset(out, 0, sizeof(*out));
    p.out = out;
    p.mouse_level = -1;

    size_t i = 0;
    while (i < len) {
        uint8_t prefix = report_map[i];

        if (prefix == HID_ITEM_LONG) {
            /* Long item: 0xFE, bDataSize, bLongItemTag, <bDataSize bytes>. */
            if (i + 1 >= len) {
                break;
            }
            uint8_t dsize = report_map[i + 1];
            i += (size_t)3 + dsize;
            continue;
        }

        uint8_t bsize = prefix & 0x03;
        uint8_t data_len = (bsize == 3) ? 4 : bsize;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;

        if (i + 1 + data_len > len) {
            break; /* truncated item -- stop, keep whatever we found */
        }
        const uint8_t *data = &report_map[i + 1];

        switch (type) {
        case HID_TYPE_MAIN:
            handle_main(&p, tag, data, data_len);
            break;
        case HID_TYPE_GLOBAL:
            handle_global(&p, tag, data, data_len);
            break;
        case HID_TYPE_LOCAL:
            handle_local(&p, tag, data, data_len);
            break;
        default:
            break;
        }

        i += (size_t)1 + data_len;
    }

    /* A usable pointer needs at least relative X and Y, and the descriptor must
     * be well-formed (every Collection closed -- an unbalanced map means we may
     * have mis-tracked where the mouse collection ends). */
    out->valid = (out->x.bit_size != 0 && out->y.bit_size != 0 && p.depth == 0);
    return out->valid ? 0 : -1;
}
