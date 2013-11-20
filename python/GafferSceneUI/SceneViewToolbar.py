##########################################################################
#  
#  Copyright (c) 2013, Image Engine Design Inc. All rights reserved.
#  
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are
#  met:
#  
#      * Redistributions of source code must retain the above
#        copyright notice, this list of conditions and the following
#        disclaimer.
#  
#      * Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials provided with
#        the distribution.
#  
#      * Neither the name of John Haddon nor the names of
#        any other contributors to this software may be used to endorse or
#        promote products derived from this software without specific prior
#        written permission.
#  
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
#  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
#  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
#  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
#  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
#  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
#  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
#  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#  
##########################################################################

import IECore

import Gaffer

import GafferUI
import GafferSceneUI

class _ExpansionPlugValueWidget( GafferUI.PlugValueWidget ) :

	def __init__( self, plug, **kw ) :
	
		menu = GafferUI.Menu( Gaffer.WeakMethod( self.__menuDefinition ) )
		menuButton = GafferUI.MenuButton( menu=menu, image = "expansion.png", hasFrame=False )
	
		GafferUI.PlugValueWidget.__init__( self, menuButton, plug, **kw )
	
	def hasLabel( self ) :
	
		return True
		
	def _updateFromPlug( self ) :
	
		pass
		
	def __menuDefinition( self ) :
	
		expandAll = bool( self.getPlug().getValue() )
	
		m = IECore.MenuDefinition()
		m.append( "/Expand Selection", { "command" : self.getPlug().node().expandSelection, "active" : not expandAll, "shortCut" : "Down" } )
		m.append( "/Expand Selection Fully", { "command" : IECore.curry( self.getPlug().node().expandSelection, depth = 999 ), "active" : not expandAll, "shortCut" : "Shift+Down" } )
		m.append( "/Collapse Selection", { "command" : self.getPlug().node().collapseSelection, "active" : not expandAll, "shortCut" : "Up" } )
		m.append( "/Expand All Divider", { "divider" : True } )
		m.append( "/Expand All", { "checkBox" : expandAll, "command" : Gaffer.WeakMethod( self.__toggleMinimumExpansionDepth ) } )
		
		return m
	
	def __toggleMinimumExpansionDepth( self, *unused ) :
	
		self.getPlug().setValue( 0 if self.getPlug().getValue() else 999 )
		
GafferUI.PlugValueWidget.registerCreator( GafferSceneUI.SceneView.staticTypeId(), "minimumExpansionDepth", _ExpansionPlugValueWidget )
