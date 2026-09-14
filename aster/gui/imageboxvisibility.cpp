#include "imagebox.h"
#include "private/imageboxpresentation.h"

namespace aster::gui {
void ImageBox::suspendForHide() {
    suspended_ = true;
    resumePending_ = resumePending_ || state_ == ImageBoxState::Loading;
    invalidateRequest();
    presentation_->syncState(ImageBoxState::Empty);
    if (config_.offscreenPolicy_ != OffscreenPolicy::Keep) {
        resumePending_ = resumePending_ || !presentation_->image().isNull();
        if (config_.offscreenPolicy_ == OffscreenPolicy::ReleaseImage)
            presentation_->clear();
        else
            presentation_->releaseHandle();
    }
    if (state_ == ImageBoxState::Loading || (config_.offscreenPolicy_ == OffscreenPolicy::ReleaseImage && state_ == ImageBoxState::Ready))
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready, true);
}

void ImageBox::resumeAfterShow() {
    suspended_ = false;
    if (resumePending_) {
        startRequest();
        return;
    }
    presentation_->syncState(state_);
    scheduleSizeRequest();
}
} // namespace aster::gui
