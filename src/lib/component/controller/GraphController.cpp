#include "component/controller/GraphController.h"

#include <set>

#include "utility/logging/logging.h"

#include "component/view/graphElements/GraphEdge.h"
#include "component/view/graphElements/GraphNode.h"
#include "component/view/GraphView.h"
#include "component/view/GraphViewStyle.h"
#include "data/access/StorageAccess.h"
#include "data/graph/Graph.h"
#include "data/graph/token_component/TokenComponentAggregation.h"

GraphController::GraphController(StorageAccess* storageAccess)
	: m_storageAccess(storageAccess)
{
}

GraphController::~GraphController()
{
}

void GraphController::handleMessage(MessageActivateTokens* message)
{
	if (message->isEdge && message->tokenIds.size() == 1)
	{
		m_currentActiveTokenIds = m_activeTokenIds;
		m_currentActiveTokenIds.push_back(message->tokenIds[0]);

		setActiveAndVisibility(m_currentActiveTokenIds);
		getView()->rebuildGraph(nullptr, m_dummyNodes, m_dummyEdges);
		return;
	}

	if (message->isAggregation)
	{
		m_activeTokenIds.clear();
		m_currentActiveTokenIds = message->tokenIds;

		createDummyGraphForTokenIds(m_currentActiveTokenIds);
		return;
	}

	setActiveTokenIds(message->tokenIds);
	createDummyGraphForTokenIds(m_activeTokenIds);
}

void GraphController::handleMessage(MessageFinishedParsing* message)
{
	getView()->clear();
}

void GraphController::handleMessage(MessageGraphNodeExpand* message)
{
	DummyNode* node = findDummyNodeRecursive(m_dummyNodes, message->tokenId);
	if (node)
	{
		if (node->autoExpanded)
		{
			node->autoExpanded = false;
			node->expanded = false;
		}
		else
		{
			node->expanded = !node->expanded;
		}

		setActiveAndVisibility(m_currentActiveTokenIds);
		layoutNesting();

		getView()->rebuildGraph(nullptr, m_dummyNodes, m_dummyEdges);
	}
}

void GraphController::handleMessage(MessageGraphNodeMove* message)
{
	DummyNode* node = findDummyNodeRecursive(m_dummyNodes, message->tokenId);
	if (node)
	{
		node->position = message->position;
		getView()->resizeView();
	}
}

GraphView* GraphController::getView() const
{
	return Controller::getView<GraphView>();
}

void GraphController::setActiveTokenIds(const std::vector<Id>& activeTokenIds)
{
	m_activeTokenIds = activeTokenIds;
	m_currentActiveTokenIds = activeTokenIds;
}

void GraphController::createDummyGraphForTokenIds(const std::vector<Id>& tokenIds)
{
	GraphView* view = getView();
	if (!view)
	{
		LOG_ERROR("GraphController has no associated GraphView");
		return;
	}

	std::shared_ptr<Graph> graph = m_storageAccess->getGraphForActiveTokenIds(tokenIds);

	m_dummyEdges.clear();

	std::set<Id> addedNodes;
	std::vector<DummyNode> dummyNodes;

	graph->forEachNode(
		[&addedNodes, &dummyNodes, this](Node* node)
		{
			Node* parent = node->getLastParentNode();
			Id id = parent->getId();
			if (addedNodes.find(id) != addedNodes.end())
			{
				return;
			}
			addedNodes.insert(id);

			dummyNodes.push_back(createDummyNodeTopDown(parent));
		}
	);

	m_dummyNodes = dummyNodes;

	autoExpandActiveNode(tokenIds);
	setActiveAndVisibility(tokenIds);

	layoutNesting();
	GraphLayouter::layoutSpectralPrototype(m_dummyNodes, m_dummyEdges);

	view->rebuildGraph(graph, m_dummyNodes, m_dummyEdges);
}

DummyNode GraphController::createDummyNodeTopDown(Node* node)
{
	DummyNode result;
	result.data = node;
	result.tokenId = node->getId();

	// there is a global root node with id 0 afaik, so here we actually want the one node below this global root
	Node* parent = node;
	while (parent != NULL && parent->getParentNode() != NULL)
	{
		parent = parent->getParentNode();
	}

	if (parent != NULL)
	{
		result.topLevelAncestorId = parent->getId();
	}

	DummyNode* oldNode = findDummyNodeRecursive(m_dummyNodes, node->getId());
	if (oldNode)
	{
		result.expanded = oldNode->expanded;
	}

	node->forEachChildNode(
		[node, &result, this](Node* child)
		{
			DummyNode* parent = nullptr;

			Edge* edge = child->getMemberEdge();
			TokenComponentAccess* access = edge->getComponent<TokenComponentAccess>();
			TokenComponentAccess::AccessType accessType = TokenComponentAccess::ACCESS_NONE;

			if (access)
			{
				accessType = access->getAccess();
			}
			else
			{
				if (node->isType(Node::NODE_CLASS | Node::NODE_STRUCT))
				{
					accessType = TokenComponentAccess::ACCESS_PUBLIC;
				}
				else
				{
					parent = &result;
				}
			}

			if (accessType != TokenComponentAccess::ACCESS_NONE)
			{
				for (DummyNode& dummy : result.subNodes)
				{
					if (dummy.accessType == accessType)
					{
						parent = &dummy;
						break;
					}
				}

				if (!parent)
				{
					DummyNode accessNode;
					accessNode.accessType = accessType;
					result.subNodes.push_back(accessNode);
					parent = &result.subNodes.back();
				}
			}

			parent->subNodes.push_back(createDummyNodeTopDown(child));
		}
	);

	node->forEachEdge(
		[&result, node, this](Edge* edge)
		{
			if (edge->isType(Edge::EDGE_MEMBER))
			{
				return;
			}

			for (const DummyEdge& dummy : m_dummyEdges)
			{
				if (dummy.data->getId() == edge->getId())
				{
					return;
				}
			}

			m_dummyEdges.push_back(DummyEdge(edge->getFrom()->getId(), edge->getTo()->getId(), edge));
		}
	);

	return result;
}

void GraphController::autoExpandActiveNode(const std::vector<Id>& activeTokenIds)
{
	DummyNode* node = nullptr;
	if (activeTokenIds.size() == 1)
	{
		node = findDummyNodeRecursive(m_dummyNodes, activeTokenIds[0]);
	}

	if (node && node->data->isType(Node::NODE_CLASS | Node::NODE_STRUCT))
	{
		node->autoExpanded = true;
	}
}

void GraphController::setActiveAndVisibility(const std::vector<Id>& activeTokenIds)
{
	for (DummyNode& node : m_dummyNodes)
	{
		setNodeActiveRecursive(node, activeTokenIds);
	}

	for (DummyEdge& edge : m_dummyEdges)
	{
		if (edge.data->isType(Edge::EDGE_AGGREGATION))
		{
			continue;
		}

		edge.active = false;
		if (find(activeTokenIds.begin(), activeTokenIds.end(), edge.data->getId()) != activeTokenIds.end())
		{
			edge.active = true;
		}

		DummyNode* from = findDummyNodeRecursive(m_dummyNodes, edge.ownerId);
		DummyNode* to = findDummyNodeRecursive(m_dummyNodes, edge.targetId);

		if (from && to && (from->active || to->active || edge.active))
		{
			edge.visible = true;
			from->connected = true;
			to->connected = true;
		}
	}

	for (DummyEdge& edge : m_dummyEdges)
	{
		if (!edge.data->isType(Edge::EDGE_AGGREGATION))
		{
			continue;
		}

		DummyNode* from = findDummyNodeRecursive(m_dummyNodes, edge.ownerId);
		DummyNode* to = findDummyNodeRecursive(m_dummyNodes, edge.targetId);

		if (from && to && (from->active || to->active))
		{
			TokenComponentAggregation* component = edge.data->getComponent<TokenComponentAggregation>();
			std::set<Id> ids = component->getAggregationIds();
			for (Id id : ids)
			{
				for (DummyEdge& e : m_dummyEdges)
				{
					if (e.visible && e.data->getId() == id)
					{
						component->removeAggregationId(id);
					}
				}
			}

			if (component->getAggregationCount() > 0)
			{
				edge.visible = true;
				from->aggregated = true;
				to->aggregated = true;
			}
		}
	}

	for (DummyNode& node : m_dummyNodes)
	{
		setNodeVisibilityRecursiveBottomUp(node, false);
	}
}

void GraphController::setNodeActiveRecursive(DummyNode& node, const std::vector<Id>& activeTokenIds) const
{
	node.visible = false;
	node.childVisible = false;
	node.active = false;

	if (node.isGraphNode())
	{
		node.active = find(activeTokenIds.begin(), activeTokenIds.end(), node.data->getId()) != activeTokenIds.end();
	}

	for (DummyNode& subNode : node.subNodes)
	{
		setNodeActiveRecursive(subNode, activeTokenIds);
	}
}

bool GraphController::setNodeVisibilityRecursiveBottomUp(DummyNode& node, bool aggregated) const
{
	for (DummyNode& subNode : node.subNodes)
	{
		if (setNodeVisibilityRecursiveBottomUp(subNode, aggregated | node.aggregated))
		{
			node.childVisible = true;
		}
	}

	if (node.active || node.connected || node.childVisible || (!aggregated && node.aggregated))
	{
		setNodeVisibilityRecursiveTopDown(node, false);
	}

	return node.visible;
}

void GraphController::setNodeVisibilityRecursiveTopDown(DummyNode& node, bool parentExpanded) const
{
	node.visible = true;

	if ((node.isGraphNode() && node.isExpanded()) ||
		(node.isAccessNode() && parentExpanded) ||
		(node.isGraphNode() && node.data->isType(Node::NODE_ENUM)) ||
		(node.isGraphNode() && node.active && node.data->isType(Node::NODE_NAMESPACE | Node::NODE_UNDEFINED)))
	{
		for (DummyNode& subNode : node.subNodes)
		{
			node.childVisible = true;
			setNodeVisibilityRecursiveTopDown(subNode, node.isExpanded());
		}
	}
}

void GraphController::layoutNesting()
{
	for (DummyNode& node : m_dummyNodes)
	{
		layoutNestingRecursive(node);
	}
}

void GraphController::layoutNestingRecursive(DummyNode& node) const
{
	GraphViewStyle::NodeMargins margins;

	if (node.isGraphNode())
	{
		margins = GraphViewStyle::getMarginsForNodeType(node.data->getType(), node.childVisible);
	}
	else if (node.isAccessNode())
	{
		margins = GraphViewStyle::getMarginsOfAccessNode();
	}
	else if (node.isExpandToggleNode())
	{
		margins = GraphViewStyle::getMarginsOfExpandToggleNode();
	}

	int y = 0;
	int x = 0;
	int width = margins.minWidth;
	int height = 0;

	if (node.isGraphNode())
	{
		width = margins.charWidth * node.data->getName().size();

		if (node.data->isType(Node::NODE_CLASS | Node::NODE_STRUCT) && node.subNodes.size())
		{
			addExpandToggleNode(node);
		}
	}

	// Horizontal layouting is currently not used, but left in place for experimentation.
	bool layoutHorizontal = false;

	for (DummyNode& subNode : node.subNodes)
	{
		if (!subNode.visible)
		{
			continue;
		}

		layoutNestingRecursive(subNode);

		if (subNode.isExpandToggleNode())
		{
			width += margins.spacingX + subNode.size.x;
		}
	}

	for (DummyNode& subNode : node.subNodes)
	{
		if (!subNode.visible || subNode.isExpandToggleNode())
		{
			continue;
		}

		subNode.position.x = margins.left + x;
		subNode.position.y = margins.top + y;

		if (layoutHorizontal)
		{
			x += subNode.size.x + margins.spacingX;
			if (subNode.size.y > height)
			{
				height = subNode.size.y;
			}
		}
		else
		{
			y += subNode.size.y + margins.spacingY;
			if (subNode.size.x > width)
			{
				width = subNode.size.x;
			}
		}
	}

	if (x > 0)
	{
		x -= margins.spacingX;
	}
	if (y > 0)
	{
		y -= margins.spacingY;
	}

	if (layoutHorizontal && x > width)
	{
		width = x;
	}

	node.size.x = margins.left + width + margins.right;
	node.size.y = margins.top + y + height + margins.bottom;

	for (DummyNode& subNode : node.subNodes)
	{
		if (subNode.isExpandToggleNode())
		{
			subNode.position.x = margins.left + width - subNode.size.x;
			subNode.position.y = 6;
		}
	}
}

void GraphController::addExpandToggleNode(DummyNode& node) const
{
	DummyNode expandNode;
	expandNode.visible = true;
	expandNode.expanded = node.expanded;
	expandNode.autoExpanded = node.autoExpanded;

	for (size_t i = 0; i < node.subNodes.size(); i++)
	{
		DummyNode& subNode = node.subNodes[i];

		if (subNode.isExpandToggleNode())
		{
			node.subNodes.erase(node.subNodes.begin() + i);
			i--;
			continue;
		}

		for (DummyNode& subSubNode : subNode.subNodes)
		{
			if (!subSubNode.visible)
			{
				expandNode.invisibleSubNodeCount++;
			}
		}
	}

	node.subNodes.push_back(expandNode);
}

DummyNode* GraphController::findDummyNodeRecursive(std::vector<DummyNode>& nodes, Id tokenId)
{
	for (DummyNode& node : nodes)
	{
		if (node.isGraphNode() && node.data->getId() == tokenId)
		{
			return &node;
		}

		DummyNode* result = findDummyNodeRecursive(node.subNodes, tokenId);
		if (result != nullptr)
		{
			return result;
		}
	}

	return nullptr;
}

DummyNode* GraphController::findDummyNodeAccessRecursive(
	std::vector<DummyNode>& nodes, Id parentId, TokenComponentAccess::AccessType type
){
	DummyNode* node = findDummyNodeRecursive(nodes, parentId);
	if (node)
	{
		for (DummyNode& subNode : node->subNodes)
		{
			if (subNode.isAccessNode() && subNode.accessType == type)
			{
				return &subNode;
			}
		}
	}
	return nullptr;
}
