import assert from "node:assert/strict";
import {prepareDefinition} from "zigbee-herdsman-converters";
import definition from "./profalux-gateway.mjs";
const prepared = prepareDefinition(definition);
assert.equal(prepared.meta.multiEndpoint, true);
assert.deepEqual(prepared.endpoint({}), {volet_1: 10, volet_2: 11, volet_3: 12, volet_4: 13});
const converter = definition.toZigbee[0];
assert.equal(definition.fromZigbee.length, 0);
assert.equal(converter.convertGet, undefined);
assert.deepEqual(converter.key, ["state"]);
for (const [i, expose] of definition.exposes.entries()) {
    assert.equal(expose.endpoint, `volet_${i + 1}`);
    assert.equal(expose.features.length, 1);
    assert.equal(expose.features[0].access, 2); // SET only
    assert.equal(expose.features[0].property, `state_volet_${i + 1}`);
    for (const [value, command] of [["OPEN", "upOpen"], ["CLOSE", "downClose"], ["STOP", "stop"]]) {
        const calls = [];
        const entity = {command: async (...args) => calls.push(args)};
        const result = await converter.convertSet(entity, "state", value, {endpoint_name: expose.endpoint});
        assert.equal(result, undefined); // No fictitious state
        assert.deepEqual(calls, [["closuresWindowCovering", command, {}, {disableDefaultResponse: false}]]);
    }
}
const entity = {command: async () => {throw new Error("unexpected send");}};
await assert.rejects(converter.convertSet(entity, "state", "50", {endpoint_name: "volet_1"}), /Only/);
await assert.rejects(converter.convertSet(entity, "state", "OPEN", {}), /Select/);
await assert.rejects(converter.convertSet({command: async () => {throw new Error("delivery failed");}}, "state", "STOP", {endpoint_name: "volet_1"}), /delivery failed/);
console.log("PASS: 12 endpoint commands, SET-only exposes, invalid commands and delivery failures; no optimistic state");
