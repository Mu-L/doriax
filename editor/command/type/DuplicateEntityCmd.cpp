#include "DuplicateEntityCmd.h"

#include "Stream.h"
#include "Out.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace doriax;

editor::DuplicateEntityCmd::DuplicateEntityCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities){
    this->project = project;
    this->sceneId = sceneId;
    this->sourceEntities = entities;
    this->wasModified = project->getScene(sceneId)->isModified;
}

namespace {
void reserveDuplicateEntityIds(YAML::Node node, EntityRegistry* registry, std::unordered_map<Entity, Entity>& entityMap) {
    if (!node || !node.IsMap()) return;

    if (node["entity"]) {
        Entity oldEntity = node["entity"].as<Entity>();
        Entity newEntity = registry->createUserEntity();
        registry->destroyEntity(newEntity);
        node["entity"] = newEntity;
        entityMap[oldEntity] = newEntity;
    }

    for (const char* key : {"members", "children", "bundleLocalEntities"}) {
        if (node[key] && node[key].IsSequence()) {
            for (auto child : node[key]) {
                reserveDuplicateEntityIds(child, registry, entityMap);
            }
        }
    }
}

void remapDuplicateLocalRefs(YAML::Node node, const std::unordered_map<Entity, Entity>& entityMap) {
    if (!node || !node.IsMap()) return;

    if (node["bundleLocalProperties"] && node["bundleLocalProperties"].IsSequence()) {
        for (auto entry : node["bundleLocalProperties"]) {
            for (const char* key : {"refEntity", "refBundleRoot"}) {
                if (!entry[key]) continue;
                auto mapped = entityMap.find(entry[key].as<Entity>());
                if (mapped != entityMap.end()) entry[key] = mapped->second;
            }
        }
    }

    for (const char* key : {"members", "children", "bundleLocalEntities"}) {
        if (node[key] && node[key].IsSequence()) {
            for (auto child : node[key]) {
                remapDuplicateLocalRefs(child, entityMap);
            }
        }
    }
}
}

bool editor::DuplicateEntityCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (!sceneProject || sourceEntities.empty()){
        return false;
    }

    Scene* scene = sceneProject->scene;

    std::vector<Entity> topLevel = Project::getTopLevelEntities(scene, sourceEntities);
    if (topLevel.empty()){
        return false;
    }

    // Record parents of source entities
    std::vector<Entity> sourceParents;
    for (Entity entity : topLevel) {
        Transform* transform = scene->findComponent<Transform>(entity);
        sourceParents.push_back(transform ? transform->parent : NULL_ENTITY);
    }

    // Encode, reserve replacement IDs, then decode the duplicates.
    // Bundle members (non-root) are skipped by the encoder when project context is set,
    // so encode them without project context to treat as plain entities.
    // They become bundle local entities automatically at save time.
    auto encodeEntityLocal = [&](Entity e) {
        bool isBundleMember = !project->findEntityBundlePathFor(sceneId, e).empty()
                              && !scene->findComponent<BundleComponent>(e);
        return Stream::encodeEntity(e, scene, isBundleMember ? nullptr : project, isBundleMember ? nullptr : sceneProject);
    };

    YAML::Node encoded;
    if (topLevel.size() == 1) {
        encoded = encodeEntityLocal(topLevel[0]);
    } else {
        encoded["type"] = "EntityBundle";
        YAML::Node membersNode(YAML::NodeType::Sequence);
        for (Entity entity : topLevel) {
            membersNode.push_back(encodeEntityLocal(entity));
        }
        encoded["members"] = membersNode;
    }
    std::unordered_map<Entity, Entity> duplicateEntityMap;
    reserveDuplicateEntityIds(encoded, scene, duplicateEntityMap);
    remapDuplicateLocalRefs(encoded, duplicateEntityMap);

    createdEntities = Stream::decodeEntitySelection(encoded, scene, &sceneProject->entities, project, sceneProject, NULL_ENTITY, true);
    if (createdEntities.empty()){
        return false;
    }
    project->resolvePendingBundleLocalRefs(sceneProject, true);

    // bind framebuffers of duplicated camera-linked textures
    CameraTextureLink::resolve(scene);

    // Top-level IDs were reserved explicitly, so member entities without a
    // Transform cannot be mistaken for additional roots.
    std::vector<Entity> createdTopLevel;
    for (Entity source : topLevel) {
        auto mapped = duplicateEntityMap.find(source);
        if (mapped != duplicateEntityMap.end() && scene->isEntityCreated(mapped->second)) {
            createdTopLevel.push_back(mapped->second);
        }
    }

    lastSelected = project->getSelectedEntities(sceneId);
    project->clearSelectedEntities(sceneId);

    std::unordered_set<std::string> existingNames;
    for (Entity e : sceneProject->entities) {
        existingNames.insert(scene->getEntityName(e));
    }

    // Re-parent, reorder, rename, and select top-level duplicates
    for (size_t i = 0; i < topLevel.size() && i < createdTopLevel.size(); i++) {
        Entity duplicated = createdTopLevel[i];

        if (sourceParents[i] != NULL_ENTITY)
            scene->addEntityChild(sourceParents[i], duplicated, false);

        // Insert duplicate right after source in entities list
        auto& entities = sceneProject->entities;
        auto dupIt = std::find(entities.begin(), entities.end(), duplicated);
        if (dupIt != entities.end()) {
            entities.erase(dupIt);
            auto srcIt = std::find(entities.begin(), entities.end(), topLevel[i]);
            if (srcIt != entities.end())
                entities.insert(srcIt + 1, duplicated);
        }

        std::string newName = ProjectUtils::makeUniqueEntityName(scene->getEntityName(topLevel[i]), existingNames);
        existingNames.insert(newName);
        scene->setEntityName(duplicated, newName);
        project->addSelectedEntity(sceneId, duplicated);
    }

    // Update all created entities (exclude Scene_Mesh_Reload which reloads ALL meshes)
    for (Entity entity : createdEntities) {
        Catalog::updateEntity(scene, entity, ~UpdateFlags_Scene_Mesh_Reload);
    }

    sceneProject->isModified = true;

    editor::Out::info("Duplicated %zu %s", topLevel.size(), topLevel.size() == 1 ? "entity" : "entities");

    return true;
}

void editor::DuplicateEntityCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (sceneProject){
        // Bundle instance metadata is not owned by EntityRegistry, so remove it
        // through Project before deleting any remaining plain entities.
        for (Entity entity : createdEntities) {
            if (!sceneProject->scene->isEntityCreated(entity)) continue;
            BundleComponent* bundleComponent = sceneProject->scene->findComponent<BundleComponent>(entity);
            if (!bundleComponent || bundleComponent->path.empty()) continue;

            Entity parentRoot = NULL_ENTITY;
            std::vector<Entity> parentPath;
            if (project->getBundleMemberAddress(sceneId, entity, parentRoot, parentPath)) continue;

            EntityBundle* bundle = project->getEntityBundle(bundleComponent->path);
            const EntityBundle::Instance* instance = bundle ? bundle->getInstance(sceneId, entity) : nullptr;
            if (!instance) continue;

            std::vector<Entity> members;
            for (const auto& member : instance->members) members.push_back(member.localEntity);
            project->unimportEntityBundle(sceneId, bundleComponent->path, entity, members);
        }

        // Delete created entities in reverse order
        for (auto it = createdEntities.rbegin(); it != createdEntities.rend(); ++it){
            if (sceneProject->scene->isEntityCreated(*it)) {
                DeleteEntityCmd::destroyEntity(sceneProject->scene, *it, sceneProject->entities, project, sceneId);
            }
        }

        if (!lastSelected.empty()){
            project->clearSelectedEntities(sceneId);
            for (Entity entity : lastSelected){
                project->addSelectedEntity(sceneId, entity);
            }
        }

        sceneProject->isModified = wasModified;
    }
}

bool editor::DuplicateEntityCmd::mergeWith(editor::Command* otherCommand){
    return false;
}

std::vector<Entity> editor::DuplicateEntityCmd::getCreatedEntities() const{
    return createdEntities;
}
