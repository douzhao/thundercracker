/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * This file is part of the internal implementation of the Sifteo SDK.
 * Confidential, not for redistribution.
 *
 * Copyright <c> 2011 Sifteo, Inc. All rights reserved.
 */

#include "runtime.h"
#include "cube.h"

using namespace Sifteo;

jmp_buf Runtime::jmpExit;

bool Event::dispatchInProgress;
uint32_t Event::pending;
uint32_t Event::eventCubes[EventBits::COUNT];


void Runtime::run()
{
    if (setjmp(jmpExit))
        return;

    siftmain();
}

void Runtime::exit()
{
    longjmp(jmpExit, 1);
}

void Event::dispatch()
{
    /*
     * Skip event dispatch if we're already in an event handler
     */

    if (dispatchInProgress)
        return;
    dispatchInProgress = true;

    /*
     * Process events, by type
     */

    while (pending) {
        EventBits::ID event = (EventBits::ID)Intrinsic::CLZ(pending);

		while (eventCubes[event]) {
                uint32_t slot = Intrinsic::CLZ(eventCubes[event]);
                if (event <= EventBits::LAST_CUBE_EVENT) {
                    callCubeEvent(event, slot);
                } else {
                    // blech
                }
                
                Atomic::And(eventCubes[event], ~Intrinsic::LZ(slot));
            }
        Atomic::And(pending, ~Intrinsic::LZ(event));
    }

    dispatchInProgress = false;
}
