using Newtonsoft.Json;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio.Tests;

/// <summary>The managed half of the world scene: materials, block classification, streaming policy, snapshots.</summary>
public sealed class WorldSceneTests
{
    internal static AcousticMaterialConfig ShippedConfig()
    {
        string path = Path.Combine(NativeTestEnvironment.RepoRoot(), "src", "VintageStorySteamAudio", "assets", "vssteamaudio", "config", "acousticmaterials.json");
        return JsonConvert.DeserializeObject<AcousticMaterialConfig>(File.ReadAllText(path))!;
    }

    [Fact]
    public void The_shipped_material_table_loads_cleanly()
    {
        var warnings = new List<string>();
        MaterialTable table = MaterialTable.Build(ShippedConfig(), warnings.Add);
        Assert.Empty(warnings);
        Assert.Equal("air", table.NameOf(0));
        Assert.Equal(MaterialKind.Air, table.KindOf(0));
        Assert.Equal(MaterialKind.Solid, table.KindOf(table.IdOf("stone")));
        Assert.Equal(MaterialKind.Porous, table.KindOf(table.IdOf("leaves")));
        Assert.Equal(MaterialKind.Liquid, table.KindOf(table.IdOf("water")));
        Assert.Equal(unchecked((int)0xFF8C8C8C), table.Colors[table.IdOf("stone")]);

        // Every block material the game has maps to a specific material ("Other" means generic).
        ushort generic = table.IdOf(MaterialTable.FallbackName);
        foreach (string blockMaterial in new[] { "Air", "Soil", "Gravel", "Sand", "Wood", "Leaves", "Stone", "Ore", "Water", "Snow", "Ice", "Metal", "Mantle", "Plant", "Glass", "Ceramic", "Cloth", "Lava", "Brick", "Fire", "Meta" })
        {
            Assert.NotEqual(generic, table.Resolve("game:x", blockMaterial));
        }

        Assert.Equal(generic, table.Resolve("game:x", "Other"));
    }

    [Fact]
    public void Blocks_resolve_by_code_rule_first_then_block_material_then_generic()
    {
        MaterialTable table = MaterialTable.Build(ShippedConfig());
        Assert.Equal("stone", table.NameOf(table.Resolve("game:rock-granite", "Stone")));
        Assert.Equal("cloth", table.NameOf(table.Resolve("game:hay-normal-ud", "Plant")));  // rule beats the block material
        Assert.Equal("glass", table.NameOf(table.Resolve("game:glasspane-leaded-oak-ns", "Wood")));
        Assert.Equal("air", table.NameOf(table.Resolve("game:tallgrass-eaten-free", "Plant")));
        Assert.Equal("air", table.NameOf(table.Resolve("game:groundstorage", "Ceramic")));
        Assert.Equal("generic", table.NameOf(table.Resolve("othermod:thing", "Unobtainium")));

        var custom = new AcousticMaterialConfig
        {
            Materials = { ["felt"] = new MaterialSpec { Kind = "Solid" } },
            ByCode = [new CodeRule { Code = "carpet-*", Material = "felt" }, new CodeRule { Code = "x", Material = "nonexistent" }],
        };
        var warnings = new List<string>();
        MaterialTable withRules = MaterialTable.Build(custom, warnings.Add);
        Assert.Equal("felt", withRules.NameOf(withRules.Resolve("anymod:carpet-red", "Cloth")));  // no domain: any domain
        Assert.Single(warnings);
        Assert.Contains("nonexistent", warnings[0], StringComparison.Ordinal);
    }

    [Fact]
    public void Bad_material_values_fall_back_with_a_warning()
    {
        var config = new AcousticMaterialConfig
        {
            Materials =
            {
                ["odd"] = new MaterialSpec { Kind = "Gaseous", Absorption = [0.1f, 0.2f], Scattering = 5f },
            },
        };
        var warnings = new List<string>();
        MaterialTable table = MaterialTable.Build(config, warnings.Add);
        AcousticMaterialDesc odd = table.Materials[table.IdOf("odd")];
        Assert.Equal(MaterialKind.Solid, odd.Kind);
        Assert.Equal(1f, odd.Scattering);
        Assert.Equal(2, warnings.Count);
        Assert.True(table.TryGetId("generic", out _));  // always present
    }

    [Fact]
    public void Blocks_are_classified_by_kind_and_collision_shape()
    {
        MaterialTable table = MaterialTable.Build(ShippedConfig());
        VsaBox unit = new(0, 0, 0, 1, 1, 1);
        VsaBox slab = new(0, 0, 0, 1, 0.5f, 1);

        BlockAcoustics stone = BlockClassifier.Classify(new BlockInfo("game:rock-granite", "Stone", [unit], false), table);
        Assert.Equal(CellShape.Full, stone.Shape);
        Assert.Equal("stone", table.NameOf(stone.Material));

        // Leaves have no collision boxes but fill their cell (porous).
        Assert.Equal(CellShape.Full, BlockClassifier.Classify(new BlockInfo("game:leaves-normal-oak", "Leaves", null, false), table).Shape);
        Assert.Equal(CellShape.Full, BlockClassifier.Classify(new BlockInfo("game:water-still-7", "Water", null, false), table).Shape);
        Assert.Equal(CellShape.Air, BlockClassifier.Classify(new BlockInfo("game:tallgrass-tall-free", "Plant", null, false), table).Shape);
        Assert.Equal(CellShape.Air, BlockClassifier.Classify(new BlockInfo("game:torch-up", "Wood", null, false), table).Shape);

        BlockAcoustics half = BlockClassifier.Classify(new BlockInfo("game:slab-granite-down", "Stone", [slab], false), table);
        Assert.Equal(CellShape.Partial, half.Shape);
        Assert.Equal(0.5f, half.Boxes![0].MaxY);

        // A fence post taller than its block is kept to its cell.
        BlockAcoustics fence = BlockClassifier.Classify(new BlockInfo("game:woodenfence-oak", "Wood", [new VsaBox(0.4f, 0, 0.4f, 0.6f, 1.5f, 0.6f)], false), table);
        Assert.Equal(1f, fence.Boxes![0].MaxY);

        Assert.Equal(CellShape.Dynamic, BlockClassifier.Classify(new BlockInfo("game:door-oak", "Wood", [new VsaBox(0, 0, 0.875f, 1, 1, 1)], true), table).Shape);
        // A door's filler block (upper half): its static box is a full cube, but its real shape is the door's.
        BlockAcoustics filler = BlockClassifier.Classify(new BlockInfo("game:multiblock-monolithic-0-p1-0", "Wood", [unit], false, DelegatesShape: true), table);
        Assert.Equal(CellShape.Dynamic, filler.Shape);

        // A full cube with a block entity (a chest-sized crate) is simply full.
        Assert.Equal(CellShape.Full, BlockClassifier.Classify(new BlockInfo("game:crate", "Wood", [unit], true), table).Shape);
    }

    [Fact]
    public void The_streamer_wants_full_detail_near_and_coarse_far_nearest_first()
    {
        var streamer = new ChunkStreamer(fullRadius: 1, lodRadius: 2, verticalRadius: 1, heightInChunks: 8);
        streamer.SetCentre(new ChunkKey(10, 0, 10));
        Assert.Equal(25 * 2, streamer.DesiredCount);  // 5x5 columns, y 0..1 (no chunk below 0)
        Assert.Equal(0, streamer.DesiredLod(new ChunkKey(11, 1, 9)));
        Assert.Equal(1, streamer.DesiredLod(new ChunkKey(12, 0, 10)));
        Assert.Null(streamer.DesiredLod(new ChunkKey(13, 0, 10)));
        Assert.Null(streamer.DesiredLod(new ChunkKey(10, -1, 10)));

        var next = streamer.Next(_ => true, 0).ToList();
        Assert.Equal(new ChunkKey(10, 0, 10), next[0].Key);
        Assert.Equal(50, next.Count);

        // Not loaded: skipped until it is.
        Assert.DoesNotContain(streamer.Next(k => k != new ChunkKey(10, 0, 10), 0), n => n.Key == new ChunkKey(10, 0, 10));
    }

    [Fact]
    public void The_streamer_resends_only_changed_chunks_after_a_debounce_and_drops_what_left()
    {
        var streamer = new ChunkStreamer(1, 1, 0, 8) { DebounceMs = 250 };
        streamer.SetCentre(new ChunkKey(0, 2, 0));
        foreach ((ChunkKey key, int lod) in streamer.Next(_ => true, 0).ToList())
        {
            Assert.True(streamer.Read(key, lod, 42));
        }

        Assert.Empty(streamer.Next(_ => true, 1000));
        var home = new ChunkKey(0, 2, 0);
        streamer.MarkDirty(home, 1000);
        Assert.Empty(streamer.Next(_ => true, 1100));   // debouncing
        Assert.Single(streamer.Next(_ => true, 1300));  // due
        Assert.False(streamer.Read(home, 0, 42));       // same contents: nothing to send
        Assert.Empty(streamer.Next(_ => true, 1400));
        streamer.MarkDirty(home, 1500);
        Assert.True(streamer.Read(home, 0, 43));        // changed

        // Unloaded, then out of range.
        Assert.Equal([home], streamer.Obsolete(k => k != home));
        streamer.SetCentre(new ChunkKey(5, 2, 5));
        Assert.Equal(9, streamer.Obsolete(_ => true).Count);
    }

    [Fact]
    public void Snapshot_fingerprints_follow_the_contents()
    {
        var a = new ChunkSnapshot();
        var b = new ChunkSnapshot();
        Assert.Equal(a.Fingerprint(), b.Fingerprint());
        a.Materials[123] = 4;
        Assert.NotEqual(a.Fingerprint(), b.Fingerprint());
        b.Materials[123] = 4;
        Assert.Equal(a.Fingerprint(), b.Fingerprint());
        a.AddPartial(5, 2, [new VsaBox(0, 0, 0, 1, 0.5f, 1)]);
        Assert.NotEqual(a.Fingerprint(), b.Fingerprint());
        Assert.Equal(1u, a.Partials[0].BoxCount);
        Assert.NotEqual(new ChunkSnapshot { Lod = 1 }.Fingerprint(), new ChunkSnapshot().Fingerprint());
        a.Clear();
        Assert.Empty(a.Partials);
        Assert.Equal(0, a.Materials[123]);
    }
}

/// <summary>The scene through the real engine, from managed code.</summary>
[Collection(NativeEngineGroup.Name)]
public sealed class WorldSceneEngineTests
{
    [Fact]
    public void A_snapshot_becomes_a_mesh_the_debug_view_can_read()
    {
        NativeTestEnvironment.RequireNatives();
        using AudioEngine engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam }, null);
        MaterialTable table = MaterialTable.Build(WorldSceneTests.ShippedConfig());
        engine.SetSceneMaterials(table.Materials);
        engine.SetSceneOrigin(16000 * 32, 96, 16000 * 32);

        var snapshot = new ChunkSnapshot { X = 16000, Y = 3, Z = 16000 };
        ushort stone = table.IdOf("stone");
        for (int z = 0; z < 32; z++)
        {
            for (int x = 0; x < 32; x++)
            {
                snapshot.Materials[(0 * 32 + z) * 32 + x] = stone;  // a floor
            }
        }

        snapshot.AddPartial((1 * 32 + 5) * 32 + 5, table.IdOf("wood"), [new VsaBox(0, 0, 0, 1, 0.5f, 1)]);
        engine.SetSceneChunk(snapshot);
        Assert.True(engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));

        SceneStats stats = engine.GetSceneStats();
        Assert.Equal(1, stats.Chunks);
        Assert.Equal(1, stats.MeshedChunks);
        Assert.Equal(table.Count, stats.MaterialCount);
        Assert.Equal((512000, 96, 512000), stats.Origin);
        Assert.Equal(2 + 12, stats.Triangles);  // the floor's top, and the slab's box

        ChunkMeshData? mesh = engine.GetChunkMesh(16000, 3, 16000);
        Assert.NotNull(mesh);
        Assert.Equal(mesh.Version, engine.GetChunkMeshVersion(16000, 3, 16000));
        Assert.Equal(12, mesh.Materials.Count(m => m == table.IdOf("wood")));
        Assert.Contains((16000, 3, 16000), engine.ListSceneChunks());
        Assert.Null(engine.GetChunkMesh(0, 0, 0));

        string obj = Path.Combine(Path.GetTempPath(), "vssteamaudio-managed-scene.obj");
        engine.SaveSceneObj(obj);
        Assert.Contains("usemtl stone", File.ReadAllText(obj), StringComparison.Ordinal);
        File.Delete(obj);
        File.Delete(Path.ChangeExtension(obj, ".mtl"));

        // Unchanged contents re-sent: same fingerprint; a removal empties the scene.
        engine.RemoveSceneChunk(16000, 3, 16000);
        Assert.True(engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));
        Assert.Equal(0, engine.GetSceneStats().Chunks);
    }

    [Fact]
    public void A_wall_between_listener_and_sound_is_seen_by_the_direct_simulation()
    {
        NativeTestEnvironment.RequireNatives();
        MaterialTable table = MaterialTable.Build(WorldSceneTests.ShippedConfig());
        foreach (bool enabled in new[] { true, false })
        {
            using AudioEngine engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam, DirectSimulation = enabled }, null);
            engine.SetSceneMaterials(table.Materials);
            var snapshot = new ChunkSnapshot();
            for (int y = 0; y < 32; y++)
            {
                for (int z = 0; z < 32; z++)
                {
                    snapshot.Materials[(y * 32 + z) * 32 + 10] = table.IdOf("stone");
                }
            }

            engine.SetSceneChunk(snapshot);
            Assert.True(engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));
            engine.SetListener(4, 16.5f, 16.5f, 1, 0, 0, 0, 1, 0);
            using AudioAsset tone = engine.CreatePcmAsset(NativeEngineTests.Sine(440, 48000, 4800, 0.3), 1, 48000, "tone");
            using Voice voice = engine.CreateVoice(tone, AudioBus.Sound, 1, 1, looping: true, new VoicePlacement(SpatialMode.World, 20, 16.5f, 16.5f));
            voice.Start();
            engine.RenderOffline(new float[9600 * 2]);

            IReadOnlyList<SourceDebugInfo> sources = engine.GetSimulatedSources();
            if (!enabled)
            {
                Assert.Empty(sources);
                Assert.Equal(0, engine.GetSimulationStats().Ticks);
                continue;
            }

            SourceDebugInfo s = Assert.Single(sources);
            Assert.Equal(voice.Handle, s.Voice);
            Assert.True(s.Occlusion < 0.05f);
            Assert.Equal(1, s.Crossings);
            Assert.Equal(1f, s.SolidMetres, 3);
            // Shipped stone: 17.5 dB crossing + 10 dB/m in the mid band.
            Assert.Equal(-27.5, 20 * Math.Log10(s.Gain.Mid), 1);
            Assert.True(engine.GetSimulationStats().Ticks > 3);
        }
    }
}
