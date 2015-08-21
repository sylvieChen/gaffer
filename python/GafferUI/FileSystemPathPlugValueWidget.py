##########################################################################
#
#  Copyright (c) 2015, Image Engine Design Inc. All rights reserved.
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

## Supported plug metadata :
#
# - "fileSystemPathPlugValueWidget:extensions"
# - "fileSystemPathPlugValueWidget:extensionsLabel"
# - "fileSystemPathPlugValueWidget:includeSequences"
# - "fileSystemPathPlugValueWidget:includeSequenceFrameRange"
class FileSystemPathPlugValueWidget( GafferUI.PathPlugValueWidget ) :

	def __init__( self, plug, **kw ) :

		GafferUI.PathPlugValueWidget.__init__(
			self,
			plug,
			Gaffer.FileSystemPath( "/" ),
			**kw
		)

		self._updateFromPlug()

		self.__plugMetadataChangedConnection = Gaffer.Metadata.plugValueChangedSignal().connect( Gaffer.WeakMethod( self.__plugMetadataChanged ) )

	def _pathChooserDialogue( self ) :

		dialogue = GafferUI.PathPlugValueWidget._pathChooserDialogue( self )

		if Gaffer.Metadata.plugValue( self.getPlug(), "fileSystemPathPlugValueWidget:includeSequences" ) :

			columns = dialogue.pathChooserWidget().pathListingWidget().getColumns()
			columns.append( GafferUI.PathListingWidget.StandardColumn( "Frame Range", "fileSystem:frameRange" ) )
			dialogue.pathChooserWidget().pathListingWidget().setColumns( columns )

		return dialogue

	def _updateFromPlug( self ) :

		GafferUI.PathPlugValueWidget._updateFromPlug( self )

		extensions = Gaffer.Metadata.plugValue( self.getPlug(), "fileSystemPathPlugValueWidget:extensions" ) or []
		if isinstance( extensions, str ) :
			extensions = extensions.split()

		includeSequences = Gaffer.Metadata.plugValue( self.getPlug(), "fileSystemPathPlugValueWidget:includeSequences" ) or False

		self.setPath(
			Gaffer.FileSystemPath(
				str(self.getPath()),
				filter =  Gaffer.FileSystemPath.createStandardFilter(
					list( extensions ),
					Gaffer.Metadata.plugValue( self.getPlug(), "fileSystemPathPlugValueWidget:extensionsLabel" ) or "",
					includeSequenceFilter = includeSequences,
				),
				includeSequences = includeSequences,
			)
		)

	def _setPlugFromPath( self, path ) :

		if Gaffer.Metadata.plugValue( self.getPlug(), "fileSystemPathPlugValueWidget:includeSequenceFrameRange" ) :
			sequence = path.fileSequence()
			if sequence :
				sequence.frameList = IECore.FrameList.parse( path.property( "fileSystem:frameRange" ) )
				self.getPlug().setValue( str(sequence) )
				return

		GafferUI.PathPlugValueWidget._setPlugFromPath( self, path )

	def __plugMetadataChanged( self, nodeTypeId, plugPath, key, plug ) :

		if self.getPlug() is None :
			return

		if plug is not None and not plug.isSame( self.getPlug() ) :
			return

		if not self.getPlug().node().isInstanceOf( nodeTypeId ) :
			return

		if key.startswith( "fileSystemPathPlugValueWidget:" ) :
			self._updateFromPlug()
