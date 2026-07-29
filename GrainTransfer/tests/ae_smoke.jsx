/*  ae_smoke.jsx — 2-minute in-AE smoke test for Grain Transfer.

    Run inside After Effects (File > Scripts > Run Script File...).
    Builds a throwaway project: a noisy "grain plate" layer (Noise effect on
    a mid-grey solid) and a gradient target (Ramp), applies Grain Transfer
    to the target with the plate as Grain Source, then saves three
    consecutive frames as PNGs next to this script.

    What to check in the PNGs / viewer:
      1. The effect loads (no missing-effect warning) and renders.
      2. Grain appears on the gradient and matches the plate's character.
      3. The three frames differ (grain does not freeze).
      4. Preview Grain Only shows clean decorrelated grain, no plate
         content ghosted into it.
      5. No seams/tiling pattern at default Size.
*/

(function () {
    var MATCH_NAME = "MM Grain Transfer";

    app.beginUndoGroup("GrainTransfer smoke");

    var proj = app.newProject();
    var comp = proj.items.addComp("GT_Smoke", 1280, 720, 1.0, 3.0, 24);

    // Grain plate: mid-grey solid + Noise (AE's basic noise ≈ fine grain).
    var plate = comp.layers.addSolid([0.5, 0.5, 0.5], "GrainPlate", 1280, 720, 1.0);
    var noise = plate.property("ADBE Effect Parade").addProperty("ADBE Noise2");
    if (noise) {
        // "Amount of Noise" — property 1
        noise.property(1).setValue(20);
    }

    // Target: gradient solid via Ramp.
    var target = comp.layers.addSolid([0.5, 0.5, 0.5], "Target", 1280, 720, 1.0);
    var ramp = target.property("ADBE Effect Parade").addProperty("ADBE Ramp");
    // ADBE Ramp defaults: black→white top→bottom — fine as-is.

    var gt = target.property("ADBE Effect Parade").addProperty(MATCH_NAME);
    if (!gt) {
        alert("FAIL: effect '" + MATCH_NAME + "' not found — plugin not loaded?");
        return;
    }
    // Param 1 is Grain Source (layer). Set it to the plate layer's index.
    gt.property(1).setValue(plate.index);

    // Plate must not composite over the target — turn its video off.
    plate.enabled = false;

    var scriptFile = new File($.fileName);
    var outDir = scriptFile.parent.fsName;

    for (var f = 0; f < 3; ++f) {
        var t = f / comp.frameRate;
        comp.time = t;
        var out = new File(outDir + "/ae_smoke_frame" + f + ".png");
        try {
            comp.saveFrameToPng(t, out);
        } catch (e) {
            alert("saveFrameToPng failed (AE too old?): " + e.toString());
            break;
        }
    }

    app.endUndoGroup();
    alert("GrainTransfer smoke done — check ae_smoke_frame0..2.png next to this script.\n" +
          "Frames must show plate-matched, non-frozen, seam-free grain on the gradient.");
})();
