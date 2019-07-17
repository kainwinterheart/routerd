#include "main.hpp"
#include <routerd_lib/handlers/proxy.hpp>
#include <ac-library/http/server/server.hpp>
#include <ac-library/http/router/router.hpp>
#include <stdlib.h>
#include <iostream>
#include <ac-common/file.hpp>
#include <unordered_set>
#include <utility>
#include <sstream>

namespace {
    nlohmann::json ParseConfig(const std::string& path) {
        NAC::TFile configFile(path);

        if (!configFile) {
            std::cerr << "Failed to open " << path << std::endl;
            return nullptr;
        }

        return nlohmann::json::parse(configFile.Data(), configFile.Data() + configFile.Size());
    }
}

namespace NAC {
    TRouterDRequestFactoryFactory DefaultRouterDRequestFactoryFactory() {
        return [](const nlohmann::json& config) {
            return [args = TRouterDRequest::TArgs::FromConfig(config)](
                std::shared_ptr<NHTTPLikeParser::TParsedData> data,
                const NHTTPServer::TResponder& responder
            ) {
                return (NHTTP::TRequest*)(new TRouterDRequest(args, data, responder));
            };
        };
    }

    int RouterDMain(const std::string& configPath) {
        return RouterDMain(configPath, DefaultRouterDRequestFactoryFactory());
    }

    int RouterDMain(
        const std::string& configPath,
        TRouterDRequestFactoryFactory&& requestFactoryFactory
    ) {
        auto&& config = ParseConfig(configPath);
        const std::string bind4((config.count("bind4") > 0) ? config["bind4"].get<std::string>() : "");
        const std::string bind6((config.count("bind6") > 0) ? config["bind6"].get<std::string>() : "");
        std::unordered_map<std::string, std::vector<TServiceHost>> hosts;

        for (const auto& spec : config["hosts"].get<std::unordered_map<std::string, std::vector<std::string>>>()) {
            if (spec.second.empty()) {
                std::cerr << spec.first << " has no hosts" << std::endl;
                return 1;
            }

            auto&& hosts_ = hosts[spec.first];
            hosts_.reserve(spec.second.size());

            for (const auto& host : spec.second) {
                const ssize_t colon(host.rfind(':'));

                if (colon < 0) {
                    std::cerr << spec.first << ": " << host << " has no port specified" << std::endl;
                    return 1;
                }

                std::stringstream ss;
                unsigned short port;
                ss << host.data() + colon + 1;
                ss >> port;

                hosts_.emplace_back(TServiceHost {
                    .Addr = std::string(host.data(), colon),
                    .Port = port
                });
            }
        }

        std::unordered_map<std::string, std::shared_ptr<NHTTPHandler::THandler>> graphs;

        for (const auto& graph : config["graphs"].get<std::unordered_map<std::string, nlohmann::json>>()) {
            const auto& data = graph.second;
            TRouterDGraph::TTree tree;
            TRouterDGraph compiledGraph;

            for (const auto& service_ : data["services"].get<std::vector<nlohmann::json>>()) {
                TService service;

                if (service_.is_string()) {
                    service.Name = service_.get<std::string>();
                    service.HostsFrom = service.Name;

                } else {
                    service.Name = service_["name"].get<std::string>();

                    if (service_.count("hosts_from") > 0) {
                        service.HostsFrom = service_["hosts_from"].get<std::string>();

                    } else {
                        service.HostsFrom = service.Name;
                    }

                    if (service_.count("path") > 0) {
                        service.Path = service_["path"].get<std::string>();
                    }
                }

                if (hosts.count(service.HostsFrom) == 0) {
                    std::cerr << graph.first << ": unknown hosts group: " << service.HostsFrom << std::endl;
                    return 1;
                }

                if (compiledGraph.Services.count(service.Name) > 0) {
                    std::cerr << graph.first << ": service already present: " << service.Name << std::endl;
                    return 1;
                }

                tree.emplace(service.Name, decltype(tree)::mapped_type());
                compiledGraph.Services.emplace(service.Name, std::move(service));
            }

            if (data.count("deps") > 0) {
                TRouterDGraph::TTree reverseTree;

                for (const auto& dep : data["deps"].get<std::vector<nlohmann::json>>()) {
                    const auto& a = dep["a"].get<std::string>();
                    const auto& b = dep["b"].get<std::string>();

                    if (a == b) {
                        std::cerr << graph.first << ": " << a << " depends on itself" << std::endl;
                        return 1;
                    }

                    if (compiledGraph.Services.count(a) == 0) {
                        std::cerr << graph.first << ": unknown service in dependency: " << a << std::endl;
                        return 1;
                    }

                    if (compiledGraph.Services.count(b) == 0) {
                        std::cerr << graph.first << ": unknown service in dependency: " << b << std::endl;
                        return 1;
                    }

                    tree[a].insert(b);
                    reverseTree[b].insert(a);
                }

                compiledGraph.Tree = tree;
                compiledGraph.ReverseTree = reverseTree;

                while (!tree.empty()) {
                    std::vector<std::string> noDeps;

                    for (auto&& it : tree) {
                        if (!it.second.empty()) {
                            continue;
                        }

                        noDeps.push_back(it.first);
                    }

                    if (noDeps.empty()) {
                        std::cerr << graph.first << ": cycle in dependencies" << std::endl;
                        return 1;
                    }

                    for (const auto& it1 : noDeps) {
                        if (reverseTree.count(it1) > 0) {
                            for (const auto& it2 : reverseTree.at(it1)) {
                                tree.at(it2).erase(it1);
                            }

                            reverseTree.erase(it1);
                        }

                        tree.erase(it1);
                    }
                }

            } else {
                compiledGraph.Tree = tree;
            }

            graphs.emplace(graph.first, std::make_shared<TRouterDProxyHandler>(hosts, std::move(compiledGraph)));
        }

        NHTTPRouter::TRouter router;

        for (const auto& route : config["routes"].get<std::vector<nlohmann::json>>()) {
            router.Add(route["r"].get<std::string>(), graphs.at(route["g"].get<std::string>()));
        }

        auto&& requestFactory = requestFactoryFactory(config);
        NHTTPServer::TServer::TArgs args;

        args.BindIP4 = (bind4.empty() ? nullptr : bind4.c_str());
        args.BindIP6 = (bind6.empty() ? nullptr : bind6.c_str());
        args.BindPort6 = args.BindPort4 = config["port"].get<unsigned short>();
        args.ThreadCount = ((config.count("threads") > 0) ? config["threads"].get<size_t>() : 10);
        args.ClientArgsFactory = [&router, &requestFactory]() {
            return new NHTTPServer::TClient::TArgs(router, std::forward<NHTTPServer::TClient::TArgs::TRequestFactory>(requestFactory));
        };

        NHTTPServer::TServer(args, router).Run();

        return 0;
    }
}
