RESULT: rn-light-client-profile sha=2dc17c3207ab9a5054b3ebf679624ca9d55237aa tier: heavy

Head: 2dc17c3207ab9a5054b3ebf679624ca9d55237aa
Branch: sprint/rn-light-client-profile.w8.g1.e1
Verdict: done

Public rocketnet_server_light_config and rocketnet_client_light_config fill the defaults, then the 10 Hz one-MTU geometry. No new config field. The default config functions are unchanged. load.c calls those two functions when LOAD_PROFILE=light.

Create-block bytes, pinned by the unit tests and by the plan line: a server at max_clients 1000 is 955710336 bytes, and a client is 3063168 bytes.

Gate on amd-1 at 2dc17c32. make -j8, python3 tools/check_exports.py --sources-only, make roadmap-check, ./bin/test, and ./bin/functional all exited 0. Functional ended ALL TESTS PASSED.

LOAD_PROFILE=light LOAD_PLAN_ONLY=1 ./bin/load 1000 60 exited 0 and printed:

load_plan: clients=1000 profile=light client_public_required_bytes=3063168 server_public_required_bytes=955710336 total_public_required_bytes=4018878336 public_memory_scope=create_only transport_selection=unresolved_before_start term_status_code=0 reason=none verdict=PASS

That same process log also recorded one gso server grant and a client handshake before the plan line. The plan line is the figure this card quotes.

The names rocketnet_client_light_config and rocketnet_server_light_config are absent at 221e3bee, so the three new cases do not exist there.

The packet-word scan stays green. The geometry lines use phrases the existing allowlist already covers. tests/test_unit_client_server_say_messages.cpp was not edited.
