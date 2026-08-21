--- TasomachiVR - first person VR for Tasomachi (UE4.25, Mixamo-named skeleton).
---
--- The game is the UE4 ThirdPerson template grown into a real game: the camera is a
--- CameraComponent on a SpringArmComponent, both components of the possessed pawn. So
--- unlike a game with a dedicated camera actor, there is nothing to fight - the game's
--- shot is simply discarded in the stereo view callback, which runs after the game is
--- done and before UEVR applies its VR transforms.
---
--- Two pawns are playable and this script treats them alike on purpose:
---   ThirdPersonCharacter_C   on foot, mesh component "CharacterMesh0"
---   BP_pawn_Plane_C          the flying boat that opens the game, mesh component
---                            "SK_Pc_01" - the same character, same skeleton, standing
---                            on the deck
--- Both carry exactly one skeletal mesh with a "Head" bone and one spring arm, so
--- everything below is found by component class rather than by name. The C++ plugin is
--- the half that does need to tell them apart, because only one of them steers with
--- ControlRotation.
---
--- Nothing the engine exposes is assumed: every call is guarded, because this sandbox
--- has no io library and a stubbed os, so a silent failure is invisible otherwise.

local api = uevr.api

local config = {
    -- Mixamo naming without the "mixamorig:" prefix. Capital H: "head" does not exist.
    bone = "Head",

    -- HANDOVER: the C++ plugin (TasomachiVR.dll) owns everything rotational - view
    -- rotation, snap turn, HMD yaw, body yaw, and the forward eye offset, which needs
    -- the headset yaw this side cannot see. What is left here is the view POSITION and
    -- the cosmetics. The two write different fields of different structs, so callback
    -- ordering between them cannot matter.

    -- Anchor the view on the mesh COMPONENT rather than on the animated bone. The bone
    -- carries the walk cycle, and head bob is a reliable way to make people sick in VR.
    -- The bone still sets the eye *height*, but heavily filtered, so crouching and the
    -- boat's gentle idle still read while the ~5 Hz bob does not.
    --
    -- The component, not the actor: on foot the two are nearly interchangeable, but on
    -- the boat the actor origin is the hull while the character stands somewhere on the
    -- deck. The mesh component sits at her feet in both cases and, unlike the bone,
    -- carries no animation - so it is the one anchor that is right for both pawns.
    stabilize        = true,
    height_smoothing = 3.0,

    hide_body     = true,
    -- "mesh" | "bone" | "none" - see set_body_hidden for the trade-off each carries.
    hide_mode     = "mesh",
    collapse_boom = true,
}

local state = {
    pawn        = nil,
    meshes      = nil,  -- skeletal mesh components of the pawn, rebuilt when it changes
    head_mesh   = nil,  -- the one that actually has the head bone
    booms       = nil,  -- spring arm components of the pawn
    hidden      = false,
    gameplay    = false,
    eye_height  = nil,  -- filtered head-above-component offset
}

local d = {
    ticks      = 0,
    pawn_class = "-",
    socket     = "-",
    anchor     = "-",
    components = "-",
    hide       = "-",
    boom       = "-",
    readback   = "-",
}

local emit = { count = 0, max = 10, last = "", next_beat = 1 }


local function try(fn, ...)
    local ok, res = pcall(fn, ...)
    if ok then
        return res
    end
    return nil
end

local function object_name(o)
    if o == nil then
        return "nil"
    end
    return try(function() return o:get_full_name() end) or "<unnamed>"
end

--- Last path segment of a UObject name, so the log stays readable.
local function short_name(o)
    local full = object_name(o)
    return full:match("([^%.]+)$") or full
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

--- UE structs use X/Y/Z, UEVR's own vector userdata uses lowercase. Accept either.
local function xyz(v)
    if v == nil then
        return nil
    end
    local x, y, z = try(function() return v.X end), try(function() return v.Y end), try(function() return v.Z end)
    if x ~= nil and y ~= nil and z ~= nil then
        return x, y, z, "upper"
    end
    x, y, z = try(function() return v.x end), try(function() return v.y end), try(function() return v.z end)
    if x ~= nil and y ~= nil and z ~= nil then
        return x, y, z, "lower"
    end
    return nil
end

local function set_xyz(v, x, y, z)
    return try(function()
        local _, _, _, casing = xyz(v)
        if casing == "upper" then
            v.X, v.Y, v.Z = x, y, z
        else
            v.x, v.y, v.z = x, y, z
        end
        return true
    end)
end

local function same_object(a, b)
    if a == nil or b == nil then
        return false
    end
    if rawequal(a, b) then
        return true
    end
    local na, nb = try(function() return a:get_full_name() end), try(function() return b:get_full_name() end)
    if na ~= nil and nb ~= nil then
        return na == nb
    end
    return a == b
end

--- Every component of the pawn of a given class. Naming the components instead would
--- mean two code paths for two pawns that need identical treatment, and would break on
--- any pawn the game adds later.
local function components_of_class(pawn, class_path)
    local list = {}
    if pawn == nil then
        return list
    end

    local cls = try(function() return api:find_uobject(class_path) end)
    local found = cls ~= nil and try(function() return pawn:K2_GetComponentsByClass(cls) end) or nil
    if found == nil then
        return list
    end

    for i in ipairs(found) do
        local c = try(function() return found[i] end)
        if c ~= nil then
            list[#list + 1] = c
        end
    end
    return list
end

--- The skeletal mesh carrying the player character, i.e. the one that has the head
--- bone. DoesSocketExist answers for bones as well as sockets, which settles it rather
--- than trusting that the first component found is the right one.
local function find_head_mesh(meshes)
    for _, m in ipairs(meshes) do
        if try(function() return m:DoesSocketExist(config.bone) end) then
            return m
        end
    end
    -- A single mesh and no working DoesSocketExist is still worth a try: GetSocketLocation
    -- falls back to the component location, which is wrong but not catastrophic, and the
    -- diagnostic below will say so.
    return meshes[1]
end

local function head_location()
    local mesh = state.head_mesh
    if mesh == nil then
        return nil
    end

    local loc = try(function() return mesh:GetSocketLocation(config.bone) end)
    local x, y, z = xyz(loc)
    if x ~= nil then
        d.socket = "GetSocketLocation"
        return x, y, z
    end

    loc = try(function() return mesh:GetBoneLocation(config.bone, 0) end)
    x, y, z = xyz(loc)
    if x ~= nil then
        d.socket = "GetBoneLocation"
        return x, y, z
    end

    d.socket = "NONE"
    return nil
end

--- The animation-free anchor: where the mesh component itself sits. See config.stabilize.
local function anchor_location()
    local mesh = state.head_mesh
    if mesh ~= nil then
        local loc = try(function() return mesh:K2_GetComponentLocation() end)
        local x, y, z = xyz(loc)
        if x ~= nil then
            d.anchor = "MeshComponent"
            return x, y, z
        end
    end

    -- Only reached if the mesh component transform is unavailable, which would be odd.
    local pawn = state.pawn
    if pawn ~= nil then
        local loc = try(function() return pawn:K2_GetActorLocation() end)
        local x, y, z = xyz(loc)
        if x ~= nil then
            d.anchor = "ActorLocation"
            return x, y, z
        end
    end

    d.anchor = "NONE"
    return nil
end

local function player_controller()
    return try(function() return api:get_player_controller(0) end)
end

local function current_view_target()
    local pc = player_controller()
    if pc == nil then
        return nil
    end
    local pcm = try(function() return pc.PlayerCameraManager end)
    if pcm == nil then
        return nil
    end
    local vt = try(function() return pcm.ViewTarget end)
    if vt == nil then
        return nil
    end
    return try(function() return vt.Target end)
end

--- Gameplay means the camera is framing the pawn we control - true on foot and on the
--- boat alike. A cutscene's CineCameraActor, PhotoMode_Camera and the menu maps all
--- fail it, and then the game keeps its own shot with only UEVR's stereo on top. This
--- mirrors the plugin's own test; both sides decide for themselves so that neither
--- depends on the other having run first.
local function compute_gameplay()
    if state.pawn == nil or state.head_mesh == nil then
        return false
    end

    local target = current_view_target()
    if target == nil then
        return false
    end
    return same_object(target, state.pawn)
end

--- Hiding the body is a compromise, because UE4 visibility is per primitive, never per
--- bone. HideBoneByName collapses the bone in the skinning itself, so every pass sees
--- the change alike - the shadow loses its head along with the view. There is no way to
--- drop one bone from the base pass only.
---
---   "mesh" : whole mesh hidden from every view, but bCastHiddenShadow keeps it casting
---            a complete and correct shadow. Nothing of the body is visible.
---   "bone" : head hidden, body still visible, shadow is headless. The head bone parents
---            the whole hair chain (Head_001..Head_006), so that goes with it.
---   "none" : leave the mesh alone
---
--- "mesh" is the default: from a viewpoint inside the head, a visible body reads worse
--- than no body at all, while a headless shadow is noticed constantly.
local function set_body_hidden(hidden)
    if config.hide_mode == "none" or state.meshes == nil then
        return
    end

    if config.hide_mode == "mesh" then
        -- Re-applied every frame, never latched: the game reasserts its own visibility
        -- on its tick, so this has to run after it and keep running.
        --
        -- Several levers are tried because they fail differently. SetRenderInMainPass
        -- is the one that matches the intent exactly: the primitive keeps feeding the
        -- shadow depth pass while dropping out of the main pass.
        local worked = {}
        for _, component in ipairs(state.meshes) do
            -- SetCastHiddenShadow reported failure in the log (shadow=0) while every
            -- other lever worked, which left the character casting no shadow at all
            -- once she was dropped from the main pass. The setter is not reachable, so
            -- the property is written directly - it is what the setter would set, and
            -- the render state is rebuilt by the visibility calls just below anyway.
            if try(function() component:SetCastHiddenShadow(true) return true end)
                or try(function() component.bCastHiddenShadow = true return true end) then
                worked.shadow = true
            end
            if try(function() component:SetRenderInMainPass(not hidden) return true end) then
                worked.mainpass = true
            end
            if try(function() component:SetVisibility(not hidden, true) return true end) then
                worked.visibility = true
            end
            if try(function() component:SetOwnerNoSee(hidden) return true end) then
                worked.ownernosee = true
            end
        end

        local levers = {}
        for _, k in ipairs({"mainpass", "visibility", "ownernosee", "shadow"}) do
            levers[#levers + 1] = k .. "=" .. (worked[k] and "1" or "0")
        end
        d.hide = "mesh[" .. table.concat(levers, ",") .. "]"
        state.hidden = hidden
        return
    end

    if state.hidden == hidden or state.head_mesh == nil then
        return
    end

    local mesh = state.head_mesh
    local ok
    if hidden then
        ok = try(function() mesh:HideBoneByName(config.bone, 0) return true end)
    else
        ok = try(function() mesh:UnHideBoneByName(config.bone) return true end)
    end

    d.hide = ok and "bone:ok" or "bone:NONE"
    if ok then
        state.hidden = hidden
    end
end

--- Collapses the boom onto its origin and kills the lag. The lag is what makes a third
--- person camera feel weighty and is exactly what must not happen to a head: any delay
--- between moving and the view following is felt immediately.
local function collapse_booms()
    if state.booms == nil or #state.booms == 0 then
        d.boom = "none found"
        return
    end

    local ok = false
    for _, boom in ipairs(state.booms) do
        if try(function()
            boom.TargetArmLength = 0.0
            boom.bEnableCameraLag = false
            boom.bEnableCameraRotationLag = false
            return true
        end) then
            ok = true
        end
    end
    d.boom = ok and ("ok x" .. #state.booms) or "NONE"
end

--- Keeps the eye at a filtered height above the anchor. Returns nil until it has
--- something to work with.
local function update_eye_height(delta)
    local _, _, az = anchor_location()
    local _, _, hz = head_location()
    if az == nil or hz == nil then
        return
    end

    local target = hz - az
    if state.eye_height == nil then
        state.eye_height = target
        return
    end

    local alpha = clamp((delta or 0.016) * config.height_smoothing, 0.0, 1.0)
    state.eye_height = state.eye_height + (target - state.eye_height) * alpha
end

local function refresh_pawn()
    local pawn = try(function() return api:get_local_pawn(0) end)

    if pawn == nil then
        state.pawn, state.meshes, state.head_mesh, state.booms = nil, nil, nil, nil
        state.hidden = false
        state.eye_height = nil
        d.pawn_class = "no pawn"
        return
    end

    if same_object(pawn, state.pawn) then
        return
    end

    state.pawn = pawn
    state.hidden = false
    -- The eye height belongs to the pawn: boarding the boat changes where the feet are
    -- relative to the head, and carrying the old value over would settle visibly.
    state.eye_height = nil

    state.meshes = components_of_class(pawn, "Class /Script/Engine.SkeletalMeshComponent")
    state.head_mesh = find_head_mesh(state.meshes)
    state.booms = components_of_class(pawn, "Class /Script/Engine.SpringArmComponent")

    local names = {}
    for _, c in ipairs(state.meshes) do
        names[#names + 1] = short_name(c)
    end
    d.components = ("%d{%s} head=%s booms=%d"):format(#state.meshes, table.concat(names, ","),
                                                      short_name(state.head_mesh), #state.booms)
    d.pawn_class = object_name(try(function() return pawn:get_class() end))
end

local function report()
    return table.concat({
        "### TasomachiVR DIAG ###",
        "ticks=" .. d.ticks,
        "pawn=" .. d.pawn_class,
        "gameplay=" .. tostring(state.gameplay),
        "components=" .. d.components,
        "socket=" .. d.socket,
        "anchor=" .. d.anchor,
        "eye_height=" .. (state.eye_height and ("%.1f"):format(state.eye_height) or "nil"),
        "hide=" .. d.hide,
        "boom=" .. d.boom,
        "readback=" .. d.readback,
    }, " ~ ")
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    d.ticks = d.ticks + 1

    refresh_pawn()
    state.gameplay = compute_gameplay()

    if state.gameplay then
        if config.collapse_boom then
            collapse_booms()
        end
        if config.stabilize then
            update_eye_height(delta)
        end
    end

    -- error() is the only way out of this sandbox: it lands in the UEVR log. Capped, and
    -- only on a change of signature, so it reports without becoming noise.
    if emit.count < emit.max then
        local sig = table.concat({d.pawn_class, tostring(state.gameplay), d.components,
                                  d.socket, d.anchor, d.hide, d.boom}, "|")
        if sig ~= emit.last or d.ticks >= emit.next_beat then
            emit.last = sig
            emit.next_beat = d.ticks + 3600
            emit.count = emit.count + 1
            error(report())
        end
    end
end)

--- After the game's tick, so its own visibility handling has already run.
uevr.sdk.callbacks.on_post_engine_tick(function()
    if not config.hide_body then
        return
    end
    -- Give the body back for cutscenes: they frame the character, so she has to be there.
    set_body_hidden(state.gameplay)
end)

uevr.sdk.callbacks.on_pre_calculate_stereo_view_offset(function(device, view_index, world_to_meters, position, rotation, is_double)
    if not state.gameplay then
        d.readback = "skipped: not gameplay"
        return
    end

    local hx, hy, hz = head_location()
    if hx == nil then
        d.readback = "skipped: no head location"
        return
    end

    local x, y, z = hx, hy, hz
    if config.stabilize then
        local ax, ay, az = anchor_location()
        if ax ~= nil and state.eye_height ~= nil then
            x, y, z = ax, ay, az + state.eye_height
        end
    end

    -- Position only. The plugin owns the view rotation and the forward eye offset,
    -- because it is the only side that knows the headset yaw.
    if set_xyz(position, x, y, z) then
        d.readback = ("z=%.1f"):format(z)
    else
        d.readback = "POS WRITE FAILED"
    end
end)

uevr.sdk.callbacks.on_script_reset(function()
    set_body_hidden(false)
    state.pawn, state.meshes, state.head_mesh, state.booms = nil, nil, nil, nil
    state.eye_height = nil
end)
