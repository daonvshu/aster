#pragma once

#include "aster/cache/pipeline/imagepipeline.h"

#include <vector>

namespace aster::cache::testing
{
class EventRecorder
{
    struct State
    {
        std::mutex mutex;
        std::vector<PipelineEvent> events;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    EventSink sink() const
    {
        return [state = state_](const PipelineEvent& event)
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->events.push_back(event);
        };
    }

    std::vector<PipelineEvent> events() const
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->events;
    }
};
}
