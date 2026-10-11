#include "wg/params/ParamResolver.hpp"

#include <algorithm>
#include <cmath>

#include "wg/core/Config.hpp"
#include "wg/core/Random.hpp"

namespace wg {

namespace {

struct PathToken {
    std::string key;
    std::vector<std::size_t> indices;
};

std::vector<PathToken> parsePath(const std::string& path) {
    std::vector<PathToken> tokens;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = std::min(path.find('.', start), path.size());
        const std::string part = path.substr(start, dot - start);
        PathToken token;
        const std::size_t bracket = part.find('[');
        token.key = part.substr(0, bracket);
        for (std::size_t b = bracket; b != std::string::npos; b = part.find('[', b + 1))
            token.indices.push_back(std::stoul(part.substr(b + 1, part.find(']', b) - b - 1)));
        tokens.push_back(std::move(token));
        start = dot + 1;
    }
    return tokens;
}

// Node at `path`, or nullptr if any step is missing.
const nlohmann::json* find(const nlohmann::json& root, const std::string& path) {
    const nlohmann::json* node = &root;
    for (const auto& token : parsePath(path)) {
        if (!node->is_object())
            return nullptr;
        const auto it = node->find(token.key);
        if (it == node->end())
            return nullptr;
        node = &*it;
        for (std::size_t index : token.indices) {
            if (!node->is_array() || index >= node->size())
                return nullptr;
            node = &(*node)[index];
        }
    }
    return node;
}

ConfigError typeError(const std::string& path, const char* expected) {
    return ConfigError("'" + path + "' must be " + expected);
}

void mergeInto(nlohmann::json& target, const nlohmann::json& patch) {
    if (target.is_object() && patch.is_object()) {
        for (const auto& [key, value] : patch.items())
            mergeInto(target[key], value);
    } else {
        target = patch;
    }
}

} // namespace

double ParamResolver::unit(const std::string& path, int index) const {
    Rng rng(deriveSeed(seed_, "param:" + path));
    for (int i = 0; i < index; ++i)
        rng.nextU64();
    return rng.nextDouble();
}

const nlohmann::json* ParamResolver::configValue(const std::string& path) {
    markConsumed(path);
    const nlohmann::json* value = find(config_, path);
    return value && !value->is_null() ? value : nullptr;
}

std::optional<double> ParamResolver::overrideNumber(const std::string& path, bool integer) {
    const nlohmann::json* value = configValue(path);
    if (!value)
        return std::nullopt;
    if (integer ? !value->is_number_integer() : !value->is_number())
        throw typeError(path, integer ? "an integer or null" : "a number or null");
    return value->get<double>();
}

void ParamResolver::record(const std::string& path, nlohmann::json value, ParamSource source) {
    entries_.push_back({path, std::move(value), source});
}

ParamSource ParamResolver::source(const std::string& path) const {
    for (const ParamEntry& entry : entries_)
        if (entry.path == path)
            return entry.source;
    return ParamSource::Default;
}

void ParamResolver::removeListElement(const std::string& listPath, const std::string& countPath, int index) {
    const std::string prefix = listPath + "[";
    std::erase_if(entries_, [&](const ParamEntry& entry) {
        return entry.path.starts_with(prefix + std::to_string(index) + "]");
    });
    int count = 0;
    for (ParamEntry& entry : entries_) {
        if (entry.path == countPath) {
            entry.value = entry.value.get<int>() - 1;
            count = entry.value.get<int>();
            continue;
        }
        if (!entry.path.starts_with(prefix))
            continue;
        const std::size_t close = entry.path.find(']', prefix.size());
        const int i = std::stoi(entry.path.substr(prefix.size(), close - prefix.size()));
        if (i > index)
            entry.path = prefix + std::to_string(i - 1) + entry.path.substr(close);
    }
    if (count == 0)
        record(listPath, nlohmann::json::array(), ParamSource::Seed);
}

// --- Random parameters -----------------------------------------------------------

double ParamResolver::sample(const std::string& path, const std::function<double(double)>& f) {
    const double drawn = f(unit(path)); // always drawn, even when overridden
    if (const auto value = overrideNumber(path, false)) {
        record(path, *value, ParamSource::Config);
        return *value;
    }
    record(path, drawn, ParamSource::Seed);
    return drawn;
}

int ParamResolver::sampleInteger(const std::string& path, const std::function<int(double)>& f) {
    const int drawn = f(unit(path));
    if (const auto value = overrideNumber(path, true)) {
        record(path, static_cast<int>(*value), ParamSource::Config);
        return static_cast<int>(*value);
    }
    record(path, drawn, ParamSource::Seed);
    return drawn;
}

double ParamResolver::uniform(const std::string& path, double min, double max) {
    return sample(path, [&](double u) { return min + (max - min) * u; });
}

double ParamResolver::logUniform(const std::string& path, double min, double max) {
    return sample(path, [&](double u) { return min * std::pow(max / min, u); });
}

int ParamResolver::integer(const std::string& path, int min, int max) {
    return sampleInteger(path, [&](double u) { return std::min(max, min + static_cast<int>(u * (max - min + 1))); });
}

bool ParamResolver::chance(const std::string& path, double probability) {
    const bool drawn = unit(path) < probability;
    if (const nlohmann::json* value = configValue(path)) {
        if (!value->is_boolean())
            throw typeError(path, "a boolean or null");
        record(path, value->get<bool>(), ParamSource::Config);
        return value->get<bool>();
    }
    record(path, drawn, ParamSource::Seed);
    return drawn;
}

std::string ParamResolver::pick(const std::string& path, const Weights& weights,
                                const std::vector<std::string>& allowed) {
    double total = 0.0;
    for (const auto& [_, weight] : weights)
        total += std::max(0.0, weight);
    if (weights.empty() || total <= 0.0)
        throw ConfigError("'" + path + "': no option has a positive weight");

    std::string drawn = weights.back().first;
    double threshold = unit(path) * total;
    for (const auto& [name, weight] : weights) {
        threshold -= std::max(0.0, weight);
        if (threshold < 0.0) {
            drawn = name;
            break;
        }
    }

    if (const nlohmann::json* value = configValue(path)) {
        std::vector<std::string> options = allowed;
        if (options.empty())
            for (const auto& [name, _] : weights)
                options.push_back(name);
        if (!value->is_string() || std::find(options.begin(), options.end(), value->get<std::string>()) == options.end()) {
            std::string list;
            for (const auto& name : options)
                list += (list.empty() ? "\"" : ", \"") + name + "\"";
            throw typeError(path, ("one of " + list + " or null").c_str());
        }
        record(path, *value, ParamSource::Config);
        return value->get<std::string>();
    }
    record(path, drawn, ParamSource::Seed);
    return drawn;
}

IslandParams::Vec2 ParamResolver::point(const std::string& path, double minX, double maxX, double minY, double maxY) {
    IslandParams::Vec2 drawn{minX + (maxX - minX) * unit(path, 0), minY + (maxY - minY) * unit(path, 1)};
    if (const nlohmann::json* value = configValue(path)) {
        if (!value->is_array() || value->size() != 2 || !(*value)[0].is_number() || !(*value)[1].is_number())
            throw typeError(path, "an [x, y] array or null");
        drawn = {(*value)[0].get<double>(), (*value)[1].get<double>()};
        record(path, *value, ParamSource::Config);
        return drawn;
    }
    record(path, nlohmann::json::array({drawn.x, drawn.y}), ParamSource::Seed);
    return drawn;
}

// --- Fixed defaults --------------------------------------------------------------

double ParamResolver::number(const std::string& path, double fallback) {
    const auto value = overrideNumber(path, false);
    record(path, value.value_or(fallback), value ? ParamSource::Config : ParamSource::Default);
    return value.value_or(fallback);
}

int ParamResolver::whole(const std::string& path, int fallback) {
    const auto value = overrideNumber(path, true);
    const int result = value ? static_cast<int>(*value) : fallback;
    record(path, result, value ? ParamSource::Config : ParamSource::Default);
    return result;
}

bool ParamResolver::flag(const std::string& path, bool fallback) {
    const nlohmann::json* value = configValue(path);
    if (value && !value->is_boolean())
        throw typeError(path, "a boolean or null");
    const bool result = value ? value->get<bool>() : fallback;
    record(path, result, value ? ParamSource::Config : ParamSource::Default);
    return result;
}

std::string ParamResolver::text(const std::string& path, const std::string& fallback,
                                std::initializer_list<std::string_view> allowed) {
    const nlohmann::json* value = configValue(path);
    if (value) {
        const bool ok = value->is_string() &&
                        (allowed.size() == 0 ||
                         std::find(allowed.begin(), allowed.end(), value->get<std::string>()) != allowed.end());
        if (!ok) {
            std::string options;
            for (auto option : allowed)
                options += (options.empty() ? "\"" : ", \"") + std::string(option) + "\"";
            throw typeError(path, allowed.size() ? ("one of " + options).c_str() : "a string");
        }
    }
    const std::string result = value ? value->get<std::string>() : fallback;
    record(path, result, value ? ParamSource::Config : ParamSource::Default);
    return result;
}

std::optional<std::string> ParamResolver::optionalText(const std::string& path) {
    const nlohmann::json* value = configValue(path);
    if (value && !value->is_string())
        throw typeError(path, "a string or null");
    record(path, value ? *value : nlohmann::json(), value ? ParamSource::Config : ParamSource::Default);
    return value ? std::optional<std::string>(value->get<std::string>()) : std::nullopt;
}

std::vector<std::string> ParamResolver::textList(const std::string& path) {
    const nlohmann::json* value = configValue(path);
    std::vector<std::string> result;
    if (value) {
        if (!value->is_array())
            throw typeError(path, "an array of strings");
        for (const auto& item : *value) {
            if (!item.is_string())
                throw typeError(path, "an array of strings");
            result.push_back(item.get<std::string>());
        }
    }
    record(path, result, value ? ParamSource::Config : ParamSource::Default);
    return result;
}

nlohmann::json ParamResolver::object(const std::string& path, const nlohmann::json& defaults) {
    nlohmann::json result = defaults;
    const nlohmann::json* value = configValue(path);
    if (value) {
        if (!value->is_object())
            throw typeError(path, "an object or null");
        mergeInto(result, *value);
    }
    record(path, result, value ? ParamSource::Config : ParamSource::Default);
    return result;
}

nlohmann::json ParamResolver::raw(const std::string& path) {
    const nlohmann::json* value = configValue(path);
    record(path, value ? *value : nlohmann::json(), value ? ParamSource::Config : ParamSource::Default);
    return value ? *value : nlohmann::json();
}

// --- Lists -------------------------------------------------------------------------

int ParamResolver::listSize(const std::string& listPath, const std::string& countPath, int minCount, int maxCount) {
    const int drawnCount = std::min(maxCount, minCount + static_cast<int>(unit(countPath) * (maxCount - minCount + 1)));
    const auto countOverride = overrideNumber(countPath, true);

    // The list itself is not marked consumed: its elements' fields are, one by one.
    const nlohmann::json* list = find(config_, listPath);
    if (list && !list->is_null()) {
        if (!list->is_array())
            throw typeError(listPath, "an array or null");
        const int size = static_cast<int>(list->size());
        if (countOverride && static_cast<int>(*countOverride) != size)
            throw ConfigError("'" + countPath + "' (" + std::to_string(static_cast<int>(*countOverride)) +
                              ") does not match the length of '" + listPath + "' (" + std::to_string(size) + ")");
        record(countPath, size, ParamSource::Config);
        if (size == 0)
            record(listPath, nlohmann::json::array(), ParamSource::Config);
        return size;
    }
    markConsumed(listPath); // null or absent

    if (countOverride && *countOverride < 0)
        throw ConfigError("'" + countPath + "' must be >= 0");
    const int count = countOverride ? static_cast<int>(*countOverride) : drawnCount;
    record(countPath, count, countOverride ? ParamSource::Config : ParamSource::Seed);
    if (count == 0)
        record(listPath, nlohmann::json::array(), ParamSource::Seed);
    return count;
}

const nlohmann::json* ParamResolver::configList(const std::string& path) {
    const nlohmann::json* value = configValue(path);
    if (value && !value->is_array())
        throw typeError(path, "an array or null");
    return value;
}

// --- Validation ----------------------------------------------------------------------

void ParamResolver::unknownKeys(const nlohmann::json& node, const std::string& path,
                                std::vector<std::string>& out) const {
    if (consumed_.contains(path))
        return;
    if (node.is_object()) {
        for (const auto& [key, value] : node.items())
            unknownKeys(value, path.empty() ? key : path + "." + key, out);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            unknownKeys(node[i], path + "[" + std::to_string(i) + "]", out);
    } else {
        out.push_back(path);
    }
}

void ParamResolver::checkNoUnknownKeys() const {
    std::vector<std::string> unknown;
    unknownKeys(config_, "", unknown);
    if (unknown.empty())
        return;
    std::string list;
    for (const auto& path : unknown)
        list += (list.empty() ? "'" : ", '") + path + "'";
    throw ConfigError("unknown parameter" + std::string(unknown.size() > 1 ? "s " : " ") + list);
}

} // namespace wg
