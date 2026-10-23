import {presets, access} from "zigbee-herdsman-converters/lib/exposes";
import {deviceEndpoints} from "zigbee-herdsman-converters/lib/modernExtend";

const endpointNames = ["volet_1", "volet_2", "volet_3", "volet_4"];
const endpoints = Object.fromEntries(endpointNames.map((name, index) => [name, 10 + index]));

// Deliberately return no optimistic state: delivery does not measure shutter position.
const commandOnlyCover = {
    key: ["state"],
    convertSet: async (entity, key, value, meta) => {
        if (!endpointNames.includes(meta.endpoint_name)) {
            throw new Error("Select volet_1, volet_2, volet_3 or volet_4");
        }
        const command = {OPEN: "upOpen", CLOSE: "downClose", STOP: "stop"}[String(value).toUpperCase()];
        if (!command) throw new Error("Only OPEN, CLOSE and STOP are supported");
        await entity.command("closuresWindowCovering", command, {}, {disableDefaultResponse: false});
    },
};

export default {
    fingerprint: [{modelID: "ZigbeeBridge", manufacturerName: "OpenProfalux"}],
    model: "OPF-ZIGBEE-PROTO",
    vendor: "OpenProfalux",
    description: "Prototype command-only Zigbee to Profalux bridge",
    extend: [deviceEndpoints({endpoints})],
    fromZigbee: [],
    toZigbee: [commandOnlyCover],
    exposes: endpointNames.map((name) => presets.cover().setAccess("state", access.SET).withEndpoint(name)),
};
