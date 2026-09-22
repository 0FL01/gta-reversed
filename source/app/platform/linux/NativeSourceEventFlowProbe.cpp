#include "NativeSourceEventFlow.h"

#include <cstdio>
#include <cstdlib>

namespace {
int g_Checks = 0;
void Check(bool value, const char* message) {
    ++g_Checks;
    if (!value) { std::fprintf(stderr, "source-event-flow-fail: %s\n", message); std::exit(1); }
}

std::vector<NativeSourceTaskTransition> Route() {
    NativeSourceEventFlow flow;
    std::string error;
    NativeSourceGroupFlowRef group;
    Check(flow.CreateGroup(group, error) == NativeSourceFlowStatus::Ok, "create group");
    NativeSourceProducerRef leader, first, second;
    Check(flow.CreateProducer(group, 10, leader, error) == NativeSourceFlowStatus::Ok &&
        flow.CreateProducer(group, 20, first, error) == NativeSourceFlowStatus::Ok &&
        flow.CreateProducer(group, 30, second, error) == NativeSourceFlowStatus::Ok, "create ordered producers");
    Check(flow.SubmitGroup(group, {100, 1, 40, 1000, false}, error) == NativeSourceFlowStatus::Ok,
        "scanner informs group in member order");
    Check(flow.Process(error) == NativeSourceFlowStatus::Ok, "process group event");
    Check(flow.Submit(leader, {200, 2, 60, 2000, true}, error) == NativeSourceFlowStatus::Ok &&
        flow.Submit(leader, {201, 3, 60, 3000, true}, error) == NativeSourceFlowStatus::Ok,
        "script command tie inputs");
    Check(flow.Process(error) == NativeSourceFlowStatus::Ok && flow.CurrentTask(leader) == 2000,
        "script command strict-greater keeps first tie");
    Check(flow.Process(error) == NativeSourceFlowStatus::Ok && flow.CurrentTask(leader) == 3000,
        "remaining script command transition");
    Check(flow.Submit(first, {300, 4, 70, 4000, false}, error) == NativeSourceFlowStatus::Ok &&
        flow.Submit(first, {301, 5, 70, 5000, false}, error) == NativeSourceFlowStatus::Ok,
        "ordinary event tie inputs");
    Check(flow.Process(error) == NativeSourceFlowStatus::Ok && flow.CurrentTask(first) == 5000,
        "ordinary event greater-or-equal selects last tie");
    Check(flow.Process(error) == NativeSourceFlowStatus::Ok && flow.CurrentTask(first) == 4000,
        "remaining ordinary event transition");
    const auto revision = flow.Transitions().size();
    Check(flow.Submit(first, {301, 6, 80, 6000, false}, error) == NativeSourceFlowStatus::SequenceConflict &&
        flow.Transitions().size() == revision, "stale scanner sequence rejection atomic");
    return {flow.Transitions().begin(), flow.Transitions().end()};
}
}

int main() {
    const auto first = Route();
    const auto second = Route();
    Check(first == second && first.size() == 7, "repeated route deterministic");
    Check(first[0].PreviousTask == 10 && first[0].CurrentTask == 1000 &&
        first[1].PreviousTask == 20 && first[2].PreviousTask == 30,
        "group producer transition order");
    Check(first[3].CurrentTask == 2000 && first[4].CurrentTask == 3000 &&
        first[5].CurrentTask == 5000 && first[6].CurrentTask == 4000,
        "event priority transition order");
    std::printf("native-source-event-flow-ok checks=%d producers=3 group=1 events=7 script-tie=first ordinary-tie=last deterministic=twice\n",
        g_Checks);
}
