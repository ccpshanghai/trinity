/* tr2c.h - engine C interface over trinity's 2D UI rendering (API version 1).
 *
 * Lives in trinity at trinity/CApi/tr2c.h (new directory, additive only).
 * trinity exports exactly one symbol, tr2c_get_api(); everything else is reached
 * through the returned function table. carbon-ui-core takes the table as its
 * backend, so it has no link-time dependency on the backend-specific trinity
 * library (_trinity_vulkan / _trinity_metal / _trinity_dx11/12).
 *
 * Conventions (same as carbon-bind's carbon_bind.h):
 *   - every function returns tr2c_status; text of the last error per thread via last_error();
 *   - all calls on the engine boot/render thread; other threads get TR2C_E_THREAD;
 *   - strings are UTF-8 with explicit byte length, copied;
 *   - handles are opaque and refcounted; *_release drops the caller's reference;
 *   - functions taking a tr2c_frame* are only valid inside the gather callback;
 *   - no C++ exception crosses this interface: an entry that hits one returns
 *     TR2C_E_OUT_OF_MEMORY (std::bad_alloc) or TR2C_E_ENGINE (anything else).
 */
#ifndef TR2C_H
#define TR2C_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(TR2C_BUILDING)
#    define TR2C_API __declspec(dllexport)
#  else
#    define TR2C_API __declspec(dllimport)
#  endif
#else
#  define TR2C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TR2C_API_VERSION 1u

typedef int32_t tr2c_status;
enum {
    TR2C_OK = 0,
    TR2C_E_INVALID_ARG = 1,
    TR2C_E_NOT_FOUND = 3,
    TR2C_E_OUT_OF_MEMORY = 4,
    TR2C_E_ENGINE = 5,          /* trinity/Blue reported an error */
    TR2C_E_VERSION = 7,
    TR2C_E_THREAD = 14,         /* same value as CB_E_THREAD */
    TR2C_E_STATE = 15           /* e.g. frame call outside the gather callback */
};

/* Opaque handles. hook/texture/text are Blue objects (one reference each). */
typedef struct tr2c_hook tr2c_hook;                 /* Tr2CSpriteHook : Tr2SpriteObjectBase */
typedef struct tr2c_frame tr2c_frame;               /* wraps Tr2Sprite2dScene* during gather */
typedef struct tr2c_texture tr2c_texture;           /* Tr2AtlasTexture */
typedef struct tr2c_text tr2c_text;                 /* OTr2FontMeasurer, limit 0 and letter space 0 set at creation */
typedef struct tr2c_display_list tr2c_display_list; /* Tr2Sprite2dDisplayList (not refcounted) */

/* Mirrors Tr2Sprite2dVertexBase: position xyz, colour rgba, two uv pairs. */
typedef struct tr2c_vertex {
    float x, y, z;
    float r, g, b, a;
    float u0, v0, u1, v1;
} tr2c_vertex;

typedef struct tr2c_font_metrics {
    float ascender;   /* px, positive up */
    float descender;  /* px, negative */
    float line_height;/* ascender - descender, as CarbonUI's Label */
} tr2c_font_metrics;

/* Called while the scene gathers its children, on the first render and then only on
 * renders after hook_set_dirty (or a device reset, or the hook being shown again). On other
 * renders the hook draws again what the last call drew, by repeating its frame calls; so
 * call hook_set_dirty whenever the output would change. Releasing or changing a texture,
 * text or display list also makes the next render call back.
 * Return value is ignored; errors are reported through last_error. */
typedef void (*tr2c_gather_fn)(tr2c_frame* frame, void* user);

/* Values equal trinity's enums so the shim is a cast; mapping is asserted in the shim. */
enum { TR2C_EFFECT_FILL = 0, TR2C_EFFECT_FILL_AA = 1, TR2C_EFFECT_COPY = 32, TR2C_EFFECT_FONT = 43,
       TR2C_EFFECT_MODULATE = 64, TR2C_EFFECT_MASK = 65 };
enum { TR2C_BLEND_NONE = 0, TR2C_BLEND_BLEND = 1, TR2C_BLEND_ADD = 2 };
enum { TR2C_TEX_NONE = 0, TR2C_TEX_TILE_X = 0x08, TR2C_TEX_TILE_Y = 0x10 };
enum { TR2C_TEXTURE_LOADING = 0, TR2C_TEXTURE_READY = 1, TR2C_TEXTURE_FAILED = 2 };

typedef struct tr2c_api {
    uint32_t struct_size;
    uint32_t api_version;

    const char* (*last_error)(void);

    /* Init: ensures a BGRA texture atlas is registered and the font manager is
     * initialised; records the calling thread. Safe to call more than once. */
    tr2c_status (*init)(void);

    /* Attach: scene MUST be the pointer carbon-bind's cb_native_ptr returns for a
     * Tr2Sprite2dScene (its IRoot*). Any other IRoot* that is not a Tr2Sprite2dScene gets
     * TR2C_E_INVALID_ARG; a pointer that is not a Blue IRoot at all (a raw object pointer,
     * a handle, freed memory) is undefined behaviour and may crash before the type check.
     * Creates a Tr2CSpriteHook and appends it to scene.children. */
    tr2c_status (*hook_create)(void* scene, tr2c_gather_fn fn, void* user, tr2c_hook** out_hook);
    tr2c_status (*hook_set_dirty)(tr2c_hook* hook);   /* next render calls the gather callback */
    tr2c_status (*hook_release)(tr2c_hook* hook);     /* removes from scene.children */

    /* Frame (only inside the gather callback). Wraps Tr2Sprite2dScene methods. */
    tr2c_status (*push_translation)(tr2c_frame* f, float x, float y);   /* PushTranslation */
    tr2c_status (*pop_translation)(tr2c_frame* f);                      /* PopTranslation */
    tr2c_status (*push_transform)(tr2c_frame* f, const float m[16]);    /* PushTransform */
    tr2c_status (*pop_transform)(tr2c_frame* f);                        /* PopTransform */
    tr2c_status (*push_clip)(tr2c_frame* f, float x, float y, float w, float h); /* PushClipRectangle */
    tr2c_status (*pop_clip)(tr2c_frame* f);                             /* PopClipRectangle */
    tr2c_status (*set_alpha)(tr2c_frame* f, float a);                   /* SetAccumulatedAlpha */
    tr2c_status (*set_effect)(tr2c_frame* f, int32_t effect);           /* SetSpriteEffect */
    tr2c_status (*set_blend)(tr2c_frame* f, int32_t blend);             /* SetBlendmode */
    tr2c_status (*set_color)(tr2c_frame* f, float r, float g, float b, float a); /* SetColor */
    tr2c_status (*set_texture)(tr2c_frame* f, uint32_t slot, tr2c_texture* tex, uint32_t settings); /* SetTexture */
    tr2c_status (*set_texture_window)(tr2c_frame* f, uint32_t slot, float x, float y, float w, float h); /* SetTextureWindow */
    /* Quad with current state: PrepareSpriteVerts + SetSpriteVerticesUVs + RenderTriangleVerts. uv: 4 corners x (u,v), may be NULL. */
    tr2c_status (*draw_quad)(tr2c_frame* f, float x, float y, float w, float h, const float uv[8]);
    /* Triangles: PrepareTriangleVerts + RenderTriangleVerts (uint32 overload); chunks internally to GetMaxVertexCountPerDrawCall. */
    tr2c_status (*draw_triangles)(tr2c_frame* f, const tr2c_vertex* v, uint32_t vcount, const uint32_t* idx, uint32_t icount);
    /* Display lists: StartCapture(hook) / EndCapture(prev) / ReplayCapture. No nesting. end may reuse prev's buffers; *out may be NULL for an empty capture. */
    tr2c_status (*capture_begin)(tr2c_frame* f);
    tr2c_status (*capture_end)(tr2c_frame* f, tr2c_display_list* prev, tr2c_display_list** out);
    tr2c_status (*replay)(tr2c_frame* f, const tr2c_display_list* dl);
    tr2c_status (*display_list_destroy)(tr2c_display_list* dl);
    /* Device-loss generation: changes when device resources (display lists) were lost. */
    uint32_t (*device_generation)(void);

    /* Textures. */
    tr2c_status (*texture_load)(const char* res_path, uint32_t len, tr2c_texture** out);   /* BeResMan->GetResource(path,"atlas") */
    tr2c_status (*texture_create_bgra)(uint32_t w, uint32_t h, const void* pixels, uint32_t pitch, tr2c_texture** out); /* FindAtlas(BGRA)->CreateTexture, LockBuffer */
    tr2c_status (*texture_state)(tr2c_texture* t, int32_t* out_state, uint32_t* out_w, uint32_t* out_h);
    tr2c_status (*texture_release)(tr2c_texture* t);

    /* Text (Tr2FontMeasurer plus six added public methods). */
    tr2c_status (*text_create)(tr2c_text** out);
    tr2c_status (*text_set_font)(tr2c_text* t, const char* res_path, uint32_t len, float size_px); /* SetFont, SetFontSize */
    tr2c_status (*text_metrics)(tr2c_text* t, tr2c_font_metrics* out);   /* added GetFontMetrics: looks the face up itself, valid right after text_set_font */
    /* Advances for a UTF-8 run, in px: SetCursorX(0), AddCodepoints, GetPendingAdvances, CancelLastText.
     * Does not touch committed lines. Same rules as add_line (no kerning, letter space 0).
     * *out_count may be less than the number of codepoints if a glyph lookup fails. */
    tr2c_status (*text_measure)(tr2c_text* t, const char* utf8, uint32_t len, float* out_adv, uint32_t cap, uint32_t* out_count);
    tr2c_status (*text_reset)(tr2c_text* t);                             /* Reset */
    /* One laid-out line: SetCursorX(0), AddCodepoints, CommitText(x, y). y is the BASELINE (line top + ascender).
     * Codepoints, not wchar_t, so non-BMP works on Windows. */
    tr2c_status (*text_add_line)(tr2c_text* t, const char* utf8, uint32_t len, int32_t x, int32_t y);
    /* PrepareSprites only when the text or colour changed since the last draw, then SubmitSprites. Position via push_translation. */
    tr2c_status (*text_draw)(tr2c_frame* f, tr2c_text* t, float r, float g, float b, float a);
    tr2c_status (*text_release)(tr2c_text* t);
} tr2c_api;

/* The single exported entry point. Returns NULL if version is unsupported. */
TR2C_API const tr2c_api* tr2c_get_api(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif /* TR2C_H */
