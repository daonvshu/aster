#include "imagebox.h"
#include "private/imageboxpresentation.h"

#include <QThread>

#include <stdexcept>

namespace aster::gui
{
void ImageBox::setOffscreenPolicy(OffscreenPolicy policy)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (int(policy) < int(OffscreenPolicy::Keep) ||
        int(policy) > int(OffscreenPolicy::ReleaseImage))
        throw std::invalid_argument("Invalid offscreen policy");
    offscreenPolicy_ = policy;
    if (!isVisible())
        suspendForHide();
}

OffscreenPolicy ImageBox::offscreenPolicy() const
{
    return offscreenPolicy_;
}

void ImageBox::suspendForHide()
{
    suspended_ = true;
    resumePending_ = resumePending_ || state_ == ImageBoxState::Loading;
    invalidateRequest();
    presentation_->syncLoadingIndicator(false);
    if (offscreenPolicy_ != OffscreenPolicy::Keep)
    {
        resumePending_ = resumePending_ || !presentation_->image().isNull();
        if (offscreenPolicy_ == OffscreenPolicy::ReleaseImage)
            presentation_->clear();
        else
            presentation_->releaseHandle();
    }
    if (state_ == ImageBoxState::Loading ||
        (offscreenPolicy_ == OffscreenPolicy::ReleaseImage && state_ == ImageBoxState::Ready))
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready,
                 true);
}

void ImageBox::resumeAfterShow()
{
    suspended_ = false;
    if (resumePending_)
    {
        startRequest();
        return;
    }
    presentation_->syncLoadingIndicator(state_ == ImageBoxState::Loading);
    scheduleSizeRequest();
}
}
