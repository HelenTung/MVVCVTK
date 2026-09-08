#pragma once
#include <vtkRenderWindowInteractor.h>

// VTK 9.4.2 allocates timer IDs globally across interactors. The fixture may
// run after hundreds of other timers; an arbitrary 1..64 search loses its tick.
// Discover the active fixture timer once using a temporary timer's actual ID
// as a finite upper bound, then reuse the interactor-local communication slot.
inline int GetTestTimerId(vtkRenderWindowInteractor* interactor)
{
    if(!interactor)return 0;
    const int cached=interactor->GetTimerEventId();
    if(cached>0&&interactor->GetTimerDuration(cached)>0)return cached;
    const int upper=interactor->CreateOneShotTimer(3600000);
    if(upper<=0)return 0;
    if(!interactor->DestroyTimer(upper))return 0;
    for(int candidate=upper-1;candidate>0;--candidate) {
        if(interactor->GetTimerDuration(candidate)>0) {
            interactor->SetTimerEventId(candidate);return candidate;
        }
    }
    return 0;
}
