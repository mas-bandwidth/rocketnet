/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_TESTS_CHANNELS_STRESS_CONFIG_H
#define ROCKETNET_TESTS_CHANNELS_STRESS_CONFIG_H

// The message config of the wide channel stress (tests/test_channels_stress.cpp,
// bin/test_channels_1024 and bin/test_channels_10k), shared with the per-push
// unit case test_channels_stress_config_is_valid
// (tests/test_unit_channels_stress_config.cpp), so a config rule that refuses
// it fails on every push and not only in the nightly.
//
// Every channel, a reliable window and send queue that hold the reliable
// rounds in flight under loss, and no send budget, so a refusal or a drop is
// a defect and never the budget. The sent-packet map holds the whole window
// (packets_in_flight at least reliable_messages_in_flight, standard/MESSAGES.md
// m9.1, proofs/tla/RNSentMap.tla NoLiveEviction).

#define CHANNELS_STRESS_WINDOW              65536      // reliable_messages_in_flight
#define CHANNELS_STRESS_RECEIPTS_PER_UPDATE 65536      // max_message_receipts

static inline void channels_stress_message_config( int * num_channels, int * reliable_messages_in_flight, int * send_message_buffer_bytes,
                                                   int * send_queue_messages, int * packets_in_flight, int * packets_per_update,
                                                   int * unreliable_messages_per_update, int * max_message_receipts, int channels )
{
    *num_channels = channels;
    *reliable_messages_in_flight = CHANNELS_STRESS_WINDOW;
    *send_message_buffer_bytes = 32 * 1024 * 1024;
    *send_queue_messages = 65536;
    *packets_in_flight = CHANNELS_STRESS_WINDOW;
    *packets_per_update = 1024;
    *unreliable_messages_per_update = 65536;
    *max_message_receipts = CHANNELS_STRESS_RECEIPTS_PER_UPDATE;
}

#endif // ROCKETNET_TESTS_CHANNELS_STRESS_CONFIG_H
