#ifndef _BS_NET_AUDIO_H_
#define _BS_NET_AUDIO_H_

// Network audio.
//
// Server: NetAudioTee wraps a real (silent) AudioSystem. Every query is answered by the wrapped system, so the game logic sees
// exactly the same timing it would with a sound card, and every mutating call is additionally recorded into a command stream.
// No audio samples ever cross the network – the clients play the sounds from their own copy of the game files.
//
// Client: NetAudioDecoder replays the command stream on the client's real AudioSystem, translating the server's sound-instance IDs.

#include "common.h"
#include "net_buf.h"
#include "audio_system.h"

AudioSystem* NetAudioTee_create(AudioSystem* inner);
NetWriter* NetAudioTee_commands(AudioSystem* tee);
// Emits the commands that recreate the currently playing sounds for a joining client
void NetAudioTee_writeJoinState(AudioSystem* tee, NetWriter* out);

typedef struct NetAudioDecoder NetAudioDecoder;
NetAudioDecoder* NetAudioDecoder_create(AudioSystem* real);
void NetAudioDecoder_destroy(NetAudioDecoder* decoder);
bool NetAudioDecoder_run(NetAudioDecoder* decoder, const uint8_t* data, size_t len);
// Statistics: commands replayed so far and how many of them started a sound
void NetAudioDecoder_stats(const NetAudioDecoder* decoder, uint64_t* outOps, uint64_t* outSoundsStarted);

#endif /* _BS_NET_AUDIO_H_ */
