// ===========================================================================
//  NoiseForgeEditor.h -- the only channel between the panel and the plugin
//
//  The editor never touches the engine and the audio thread never touches the
//  editor. Knob moves go out as host edit gestures; the read-only display
//  values (scope ring, peak, table name, seed) are atomics inside the engine
//  reached through this interface.
// ===========================================================================

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

namespace nf {

class IParamSink {
public:
    virtual ~IParamSink() {}

    // Edit gestures, so automation records instead of being a silent write.
    virtual void onBeginEdit(int id) = 0;
    virtual void onPerformEdit(int id, double value) = 0;
    virtual void onEndEdit(int id) = 0;

    // Current value, for laying out controls and for host-driven updates.
    virtual double getNormalized(int id) = 0;

    // Read-only display feed.
    virtual void copyScope(float* dst, int n) = 0;
    virtual float peak() = 0;
    virtual float takePeak() = 0;
    virtual uint32_t scopeCount() = 0;

    // Wavetable bank. Returns false when the audio thread still holds the
    // previous bank; the editor retries on its next timer tick.
    virtual bool randomizeTables() = 0;
    virtual uint32_t tableSeed() = 0;
    virtual const char* tableName(int t) = 0;
    virtual void copyTablePreview(int table, float pos, float* dst, int n) = 0;

    virtual int activeVoices() = 0;
    virtual double sampleRate() = 0;

    // Lifecycle. The editor tells the plugin which view is live, so the plugin
    // can never hold a pointer to a view the host has already released.
    virtual void setActiveEditor(Steinberg::IPlugView* view) = 0;
};

Steinberg::IPlugView* createNoiseForgeEditor(IParamSink* sink);

// Safe from any thread: posts a window message and nothing else.
void notifyEditorParamChanged(Steinberg::IPlugView* view, int id);

} // namespace nf
