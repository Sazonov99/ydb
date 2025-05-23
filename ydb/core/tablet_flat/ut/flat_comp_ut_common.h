#pragma once

#include <ydb/core/tablet_flat/flat_comp.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <ydb/core/tablet_flat/util_fmt_line.h>

#include <ydb/core/tablet_flat/test/libs/table/test_part.h>
#include <ydb/core/tablet_flat/test/libs/table/test_comp.h>

#include <library/cpp/time_provider/time_provider.h>

namespace NKikimr {
namespace NTable {
namespace NTest {

class TSimpleBackend : public ICompactionBackend {
public:
    TSimpleBackend() {
        SwitchGen();
    }

    NIceDb::TNiceDb Begin() {
        Annex->Switch(++Step, /* require step switch */ true);
        DB.Begin({ Gen, Step }, Env.emplace());
        return DB;
    }

    void Commit() {
        DB.Commit({ Gen, Step }, true, Annex.Get());
        Env.reset();
    }

    TSnapEdge SnapshotTable(ui32 table) {
        const auto scn = DB.Head().Serial + 1;
        TTxStamp txStamp(Gen, ++Step);
        DB.SnapshotToLog(table, txStamp);
        Y_ENSURE(scn == DB.Head().Serial);
        auto chg = DB.Head(table);
        return { txStamp, chg.Epoch };
    }

    ui64 OwnerTabletId() const override {
        return TabletId;
    }

    const TScheme& DatabaseScheme() override {
        return DB.GetScheme();
    }

    TIntrusiveConstPtr<NKikimr::NTable::TRowScheme> RowScheme(ui32 table) const override {
        return DB.GetRowScheme(table);
    }

    const TScheme::TTableInfo* TableScheme(ui32 table) override {
        auto* info = DB.GetScheme().GetTableInfo(table);
        Y_ENSURE(info, "Unexpected table");
        return info;
    }

    ui64 TableMemSize(ui32 table, TEpoch epoch) override {
        return DB.GetTableMemSize(table, epoch);
    }

    TPartView TablePart(ui32 table, const TLogoBlobID& label) override {
        auto partView = DB.GetPartView(table, label);
        Y_ENSURE(partView, "Unexpected part " << label);
        return partView;
    }

    TVector<TPartView> TableParts(ui32 table) override {
        return DB.GetTableParts(table);
    }

    TVector<TIntrusiveConstPtr<TColdPart>> TableColdParts(ui32 table) override {
        return DB.GetTableColdParts(table);
    }

    const TRowVersionRanges& TableRemovedRowVersions(ui32 table) override {
        return DB.GetRemovedRowVersions(table);
    }

    ui64 BeginCompaction(THolder<TCompactionParams> params) override {
        Y_ENSURE(params);
        ui64 compactionId = NextCompactionId_++;
        StartedCompactions[compactionId] = std::move(params);
        return compactionId;
    }

    bool CancelCompaction(ui64 compactionId) override {
        return StartedCompactions.erase(compactionId) > 0;
    }

    void RequestChanges(ui32 table) override {
        Y_ENSURE(table == 1, "Unexpected table");
        ChangesRequested_ = true;
    }

    bool CheckChangesFlag() {
        return std::exchange(ChangesRequested_, false);
    }

    struct TRunCompactionResult {
        ui64 CompactionId;
        THolder<TCompactionParams> Params;
        THolder<TCompactionResult> Result;
    };

    TRunCompactionResult RunCompaction() {
        Y_ENSURE(StartedCompactions, "There are no started compactions");
        ui64 compactionId = StartedCompactions.begin()->first;
        return RunCompaction(compactionId);
    }

    TRunCompactionResult RunCompaction(ui64 compactionId) {
        auto it = StartedCompactions.find(compactionId);
        Y_ENSURE(it != StartedCompactions.end());
        auto params = std::move(it->second);
        StartedCompactions.erase(it);
        auto result = RunCompaction(params.Get());
        return { compactionId, std::move(params), std::move(result) };
    }

    THolder<TCompactionResult> RunCompaction(const TCompactionParams* params) {
        if (params->Edge.Head == TEpoch::Max()) {
            SnapshotTable(params->Table);
        }

        auto subset = DB.CompactionSubset(params->Table, params->Edge.Head, { });
        if (params->Parts) {
            subset->Flatten.insert(subset->Flatten.end(), params->Parts.begin(), params->Parts.end());
        }

        // Note: we don't compact TxStatus in these tests
        Y_ENSURE(subset->TxStatus.empty());

        Y_ENSURE(!*subset || subset->IsStickedToHead());

        const auto& scheme = DB.GetScheme();
        auto* family = scheme.DefaultFamilyFor(params->Table);
        auto* policy = scheme.GetTableInfo(params->Table)->CompactionPolicy.Get();

        NPage::TConf conf(params->IsFinal, policy->MinDataPageSize);
        conf.Group(0).BTreeIndexNodeTargetSize = policy->MinBTreeIndexNodeSize;
        conf.Group(0).BTreeIndexNodeKeysMin = policy->MinBTreeIndexNodeKeys;
        conf.UnderlayMask = params->UnderlayMask.Get();
        conf.SplitKeys = params->SplitKeys.Get();
        conf.Group(0).Codec = family->Codec;
        conf.SmallEdge = family->Small;
        conf.LargeEdge = family->Large;
        conf.MaxRows = subset->MaxRows();
        conf.ByKeyFilter = scheme.GetTableInfo(params->Table)->ByKeyFilter;

        // Don't care about moving blobs by reference
        TAutoPtr<IPages> env = new TTestEnv;

        // Template for new blobs
        TLogoBlobID logo(TabletId, Gen, ++Step, 0, 0, 0);

        auto eggs = TCompaction(env, conf)
                .WithRemovedRowVersions(DB.GetRemovedRowVersions(params->Table))
                .Do(*subset, logo);

        TVector<TPartView> parts(Reserve(eggs.Parts.size()));
        for (auto& part : eggs.Parts) {
            parts.push_back({ part, nullptr, part->Slices });
            Y_ENSURE(parts.back());
        }

        DB.Replace(params->Table, *subset, parts, { });

        return MakeHolder<TCompactionResult>(subset->Epoch(), std::move(parts));
    }

    void ApplyChanges(ui32 table, TCompactionChanges changes) {
        auto& state = TableState[table];
        for (auto& kv : changes.StateChanges) {
            if (kv.second) {
                state[kv.first] = std::move(kv.second);
            } else {
                state.erase(kv.first);
            }
        }

        for (auto& change : changes.SliceChanges) {
            auto partView = DB.GetPartView(table, change.Label);
            Y_ENSURE(partView, "Cannot find part " << change.Label);
            auto replaced = TSlices::Replace(partView.Slices, change.NewSlices);
            DB.ReplaceSlices(table, {{ change.Label, std::move(replaced) }});
        }
    }

    void SimpleMemCompaction(ui32 table) {
        TCompactionParams params;
        params.Table = table;
        params.Edge.Head = TEpoch::Max();
        RunCompaction(&params);
    }

    ui64 SimpleMemCompaction(ICompactionStrategy* strategy, bool forced = false) {
        ui64 forcedCompactionId = forced ? NextForcedCompactionId_++ : 0;
        ui64 compactionId = strategy->BeginMemCompaction(0, { 0, TEpoch::Max() }, forcedCompactionId);
        auto outcome = RunCompaction(compactionId);
        const ui32 table = outcome.Params->Table;
        auto changes = strategy->CompactionFinished(
                compactionId, std::move(outcome.Params), std::move(outcome.Result));
        ApplyChanges(table, std::move(changes));
        return forcedCompactionId;
    }

    bool SimpleTableCompaction(ui32 table, IResourceBroker* broker, ICompactionStrategy* strategy) {
        for (auto& kv : StartedCompactions) {
            if (kv.second->Table == table) {
                ui64 compactionId = kv.first;
                auto outcome = RunCompaction(compactionId);
                broker->FinishTask(outcome.Params->TaskId, EResourceStatus::Finished);
                auto changes = strategy->CompactionFinished(
                        compactionId,
                        std::move(outcome.Params),
                        std::move(outcome.Result));
                ApplyChanges(table, std::move(changes));
                return true;
            }
        }

        return false;
    }

    TString DumpKeyRanges(ui32 table, bool dumpStep = false) {
        struct TKeyRange : public TBounds {
            TEpoch Epoch;
            ui32 Step;

            TKeyRange(const TBounds& bounds, TEpoch epoch, ui32 step)
                : TBounds(bounds)
                , Epoch(epoch)
                , Step(step)
            { }

            TString ToString(const TKeyCellDefaults& keyDefaults, bool dumpStep) const {
                TStringStream s;
                Describe(s, keyDefaults);
                s << "@" << Epoch;
                if (dumpStep) {
                    s << "/" << Step;
                }
                return std::move(s.Str());
            }
        };

        const TKeyCellDefaults& keyDefaults = *DB.GetRowScheme(table)->Keys;
        auto keyRangeLess = [&keyDefaults](const TKeyRange& a, const TKeyRange& b) -> bool {
            if (auto cmp = ComparePartKeys(a.FirstKey.GetCells(), b.FirstKey.GetCells(), keyDefaults)) {
                return cmp < 0;
            }
            if (a.FirstInclusive != b.FirstInclusive) {
                return a.FirstInclusive && !b.FirstInclusive;
            }
            if (auto cmp = ComparePartKeys(a.LastKey.GetCells(), b.LastKey.GetCells(), keyDefaults)) {
                return cmp < 0;
            }
            if (a.LastInclusive != b.LastInclusive) {
                return !a.LastInclusive && b.LastInclusive;
            }
            return a.Epoch < b.Epoch;
        };

        TVector<TKeyRange> keyRanges;
        for (auto& partView : TableParts(table)) {
            for (auto& slice : *partView.Slices) {
                keyRanges.emplace_back(slice, partView->Epoch, partView->Label.Step());
            }
        }
        std::sort(keyRanges.begin(), keyRanges.end(), keyRangeLess);

        TString result;
        for (auto& keyRange : keyRanges) {
            if (result) {
                result.append(' ');
            }
            result += keyRange.ToString(keyDefaults, dumpStep);
        }
        return result;
    }

private:
    void SwitchGen() {
        ++Gen;
        Step = 0;
        Annex.Reset(new NPageCollection::TSteppedCookieAllocator(TabletId, ui64(Gen) << 32, { 0, 999 }, {{ 1, 7 }}));
    }

public:
    TDatabase DB;
    std::optional<TTestEnv> Env;
    THashMap<ui64, THolder<TCompactionParams>> StartedCompactions;
    THashMap<ui32, THashMap<ui64, TString>> TableState;
    ui64 TabletId = 123;

private:
    THolder<NPageCollection::TSteppedCookieAllocator> Annex;
    ui32 Gen = 0;
    ui32 Step = 0;

    ui64 NextCompactionId_ = 1;
    ui64 NextForcedCompactionId_ = 1001;

    bool ChangesRequested_ = false;
};

class TSimpleBroker : public IResourceBroker {
public:
    TTaskId SubmitTask(TString name, TResourceParams params, TResourceConsumer consumer) override {
        Y_UNUSED(name);
        Y_UNUSED(params);
        auto taskId = NextTaskId_++;
        Pending_[taskId] = consumer;
        return taskId;
    }

    void UpdateTask(TTaskId taskId, TResourceParams params) override {
        Y_UNUSED(taskId);
        Y_UNUSED(params);
    }

    void FinishTask(TTaskId taskId, EResourceStatus status) override {
        Y_UNUSED(status);
        Running_.erase(taskId);
    }

    bool CancelTask(TTaskId taskId) override {
        return Pending_.erase(taskId) > 0;
    }

    bool HasPending() const {
        return bool(Pending_);
    }

    bool HasRunning() const {
        return bool(Running_);
    }

    bool RunPending() {
        if (auto it = Pending_.begin(); it != Pending_.end()) {
            auto taskId = it->first;
            auto consumer = std::move(it->second);
            Pending_.erase(it);
            Running_.insert(taskId);
            consumer(taskId);
            return true;
        }
        return false;
    }

private:
    TTaskId NextTaskId_ = 1;
    THashMap<TTaskId, TResourceConsumer> Pending_;
    THashSet<TTaskId> Running_;
};

class TSimpleTime : public ITimeProvider {
public:
    TInstant Now() override {
        return Now_;
    }

    void Move(TInstant now) {
        Now_ = now;
    }

private:
    TInstant Now_;
};

struct TSimpleLogger : public NUtil::ILogger {
    NUtil::TLogLn Log(NUtil::ELnLev level) const override {
        return { nullptr, level };
    }
};

} // NTable
} // Kikimr
} // NTest
