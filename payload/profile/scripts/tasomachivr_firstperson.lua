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

    -- The eye FOLLOWS THE HEAD BONE, filtered - it is not anchored on the mesh
    -- component any more.
    --
    -- Anchoring on the component was right while the body was invisible: the component
    -- carries no animation, so it gave a rock-steady view. It becomes wrong the moment
    -- the body is drawn, because the body is animated relative to that component. The
    -- head slides sideways when walking, and a jump throws it in front of the eye.
    --
    -- So the eye tracks the bone, and the bob is removed by a low-pass with a HARD
    -- LIMIT: the filtered eye may never sit further than sway_limit from where the head
    -- really is. Small oscillations - the ~5 Hz walk cycle - are damped away, while a
    -- jump or a crouch is followed exactly, because those exceed the limit immediately.
    -- A plain low-pass cannot do both; the clamp is what separates them.
    stabilize    = true,
    -- Higher follows faster. Vertical carries almost all of the bob, so it is damped
    -- harder than horizontal.
    bob_damping  = 9.0,
    sway_damping = 22.0,
    -- Centimetres. Beyond this the filter gives up and tracks the head exactly.
    sway_limit   = 4.0,

    -- Body visibility used to live here. It is the plugin's now (see plugin/body.hpp):
    -- the Lua sandbox cannot read the ini, so a setting kept here could never be
    -- exposed on the VR settings page.
    collapse_boom = true,
}

local state = {
    pawn        = nil,
    meshes      = nil,  -- skeletal mesh components of the pawn, rebuilt when it changes
    head_mesh   = nil,  -- the one that actually has the head bone
    booms       = nil,  -- spring arm components of the pawn
    gameplay    = false,
    eye         = nil,  -- filtered world position of the eye
}

local d = {
    ticks      = 0,
    pawn_class = "-",
    socket     = "-",
    components = "-",
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

--- Moves the filtered eye towards the head bone. Returns nothing; the result lives in
--- state.eye and is read by the view callback.
local function update_eye(delta)
    local hx, hy, hz = head_location()
    if hx == nil then
        return
    end

    if state.eye == nil then
        state.eye = {x = hx, y = hy, z = hz}
        return
    end

    local dt = delta or 0.016
    local a_xy = clamp(dt * config.sway_damping, 0.0, 1.0)
    local a_z = clamp(dt * config.bob_damping, 0.0, 1.0)

    state.eye.x = state.eye.x + (hx - state.eye.x) * a_xy
    state.eye.y = state.eye.y + (hy - state.eye.y) * a_xy
    state.eye.z = state.eye.z + (hz - state.eye.z) * a_z

    -- The clamp is the point: it is what lets one filter both damp the walk cycle and
    -- follow a jump. Without it, smoothing enough to kill the bob also means the body
    -- outruns the camera whenever the character really moves.
    local limit = config.sway_limit
    state.eye.x = clamp(state.eye.x, hx - limit, hx + limit)
    state.eye.y = clamp(state.eye.y, hy - limit, hy + limit)
    state.eye.z = clamp(state.eye.z, hz - limit, hz + limit)
end

local function refresh_pawn()
    local pawn = try(function() return api:get_local_pawn(0) end)

    if pawn == nil then
        state.pawn, state.meshes, state.head_mesh, state.booms = nil, nil, nil, nil
        state.eye = nil
        d.pawn_class = "no pawn"
        return
    end

    if same_object(pawn, state.pawn) then
        return
    end

    state.pawn = pawn
    -- The eye belongs to the pawn: boarding the boat teleports the head, and carrying
    -- the old filtered position over would show as a visible slide into place.
    state.eye = nil

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
        "eye_z=" .. (state.eye and ("%.1f"):format(state.eye.z) or "nil"),
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
            update_eye(delta)
        end
    end

    -- error() is the only way out of this sandbox: it lands in the UEVR log. Capped, and
    -- only on a change of signature, so it reports without becoming noise.
    if emit.count < emit.max then
        local sig = table.concat({d.pawn_class, tostring(state.gameplay), d.components,
                                  d.socket, d.boom}, "|")
        if sig ~= emit.last or d.ticks >= emit.next_beat then
            emit.last = sig
            emit.next_beat = d.ticks + 3600
            emit.count = emit.count + 1
            error(report())
        end
    end
end)

-- The view POSITION used to be written here. It is the plugin's now (plugin/eye.cpp):
-- the wall test has to happen where the eye is computed, and two callbacks writing the
-- same struct was a race waiting to be noticed. What is left on this side is the spring
-- arm, which has nothing to do with either.

uevr.sdk.callbacks.on_script_reset(function()
    state.pawn, state.meshes, state.head_mesh, state.booms = nil, nil, nil, nil
    state.eye = nil
end)
