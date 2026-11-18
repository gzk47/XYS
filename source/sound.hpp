#pragma once

namespace sound {

enum class Sfx { Move, Confirm, Cancel, Success, Error };

// SDL audio based UI feedback. All samples are generated at init, so no extra
// assets are required. play() is non-blocking and safe to call from the main
// thread only.
bool init();
void shutdown();
void play(Sfx sfx);

}  // namespace sound
