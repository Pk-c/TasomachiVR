--- TasomachiVR - the one job left on the Lua side: collapsing the camera boom.
---
--- The game is the UE4 ThirdPerson template grown into a real game: the camera is a
--- CameraComponent on a SpringArmComponent, both components of the possessed pawn. Two
--- pawns are playable and both are treated alike, because both carry exactly one spring
--- arm:
---   ThirdPersonCharacter_C   on foot
---   BP_pawn_Plane_C          the flying boat that opens the game
--- Components are found by CLASS rather than by name, so neither pawn needs a special
--- case and a third one would work unchanged.
---
--- Everything else this file used to do now lives in the C++ plugin: the view position
--- and its filtering (plugin/eye.cpp), all rotation and turning, the body, the arms. Two
--- callbacks writing the same struct was a race waiting to be noticed, and the plugin is
--- the side that can see the headset. What is left here is the spring arm, which touches
--- neither.
---
--- Nothing the engine exposes is assumed: every call is guarded, because this sandbox has
--- no io library and a stubbed os, so a silent failure would otherwise be invisible.

local api = uevr.api

local state = {
    pawn = nil,
    booms = nil,
}

local function try(fn, ...)
    local ok, res = pcall(fn, ...)
    if ok then
        return res
    end
    return nil
end

--- UObject identity. Comparing the userdata directly is not reliable across calls, so the
--- full name is the fallback.
local function same_object(a, b)
    if a == nil or b == nil then
        return false
    end
    if rawequal(a, b) then
        return true
    end
    local na = try(function() return a:get_full_name() end)
    local nb = try(function() return b:get_full_name() end)
    if na ~= nil and nb ~= nil then
        return na == nb
    end
    return a == b
end

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

local function current_view_target()
    local pc = try(function() return api:get_player_controller(0) end)
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
--- boat alike. A cutscene's CineCameraActor, PhotoMode_Camera and the menu maps all fail
--- it, and there the game keeps its own shot with only UEVR's stereo on top. The plugin
--- runs the same test independently, so neither side depends on the other having run.
local function is_gameplay()
    return state.pawn ~= nil and same_object(current_view_target(), state.pawn)
end

--- Collapses the boom onto its origin and kills the lag. The lag is what makes a third
--- person camera feel weighty, and it is exactly what must not happen to a head: any
--- delay between moving and the view following is felt immediately, and in VR it is felt
--- as nausea.
local function collapse_booms()
    if state.booms == nil then
        return
    end
    for _, boom in ipairs(state.booms) do
        try(function()
            boom.TargetArmLength = 0.0
            boom.bEnableCameraLag = false
            boom.bEnableCameraRotationLag = false
        end)
    end
end

--- Re-read only when the pawn actually changes: K2_GetComponentsByClass allocates, and
--- doing that every frame for a value that changes twice a session would be wasteful.
local function refresh_pawn()
    local pawn = try(function() return api:get_local_pawn(0) end)

    if pawn == nil then
        state.pawn, state.booms = nil, nil
        return
    end
    if same_object(pawn, state.pawn) then
        return
    end

    state.pawn = pawn
    state.booms = components_of_class(pawn, "Class /Script/Engine.SpringArmComponent")
end

uevr.sdk.callbacks.on_pre_engine_tick(function()
    refresh_pawn()
    if is_gameplay() then
        collapse_booms()
    end
end)

uevr.sdk.callbacks.on_script_reset(function()
    state.pawn, state.booms = nil, nil
end)
