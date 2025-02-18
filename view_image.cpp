#include <map>
#include "mac_filesystem.hpp"
#include <QTextStream>
#include <QInputDialog>
#include <QFileDialog>
#include "view_image.h"
#include "ui_view_image.h"
#include "prog_interface_static_link.h"
#include <QPlainTextEdit>
#include <QFileInfo>
#include <QMessageBox>
#include <QBuffer>
#include <QImageReader>

std::map<std::string,std::string> dicom_dictionary;

bool img_command(tipl::image<float,3>& data,
                 tipl::vector<3>& vs,
                 tipl::matrix<4,4,float>& T,
                 std::string cmd,
                 std::string param1,
                 std::string,
                 std::string& error_msg)
{
    if(cmd == "image_multiplication" || cmd == "image_addition")
    {
        gz_nifti nii;
        if(!nii.load_from_file(param1.c_str()))
        {
            error_msg = "cannot open file:";
            error_msg += param1;
            return false;
        }
        tipl::image<float,3> mask;
        nii >> mask;
        if(mask.geometry() != data.geometry())
        {
            error_msg = "invalid mask file:";
            error_msg += param1;
            error_msg += " The dimension does not match:";
            std::ostringstream out;
            out << mask.geometry() << " vs " << data.geometry();
            error_msg += out.str();
            return false;
        }
        if(cmd == "image_multiplication")
            data *= mask;
        if(cmd == "image_addition")
            data += mask;
        return true;
    }
    if(cmd == "save")
    {
        gz_nifti nii;
        nii.set_image_transformation(T);
        nii.set_voxel_size(vs);
        nii << data;
        return nii.save_to_file(param1.c_str());
    }
    if(cmd == "open")
    {
        gz_nifti nii;
        nii.set_image_transformation(T);
        nii.set_voxel_size(vs);
        nii << data;
        return nii.save_to_file(param1.c_str());
    }
    return false;
}

bool view_image::command(std::string cmd,std::string param1,std::string param2)
{
    std::string error_msg;
    if(!img_command(data,vs,T,cmd,param1,param2,error_msg))
    {
        QMessageBox::information(this,"Error",error_msg.c_str());
        return false;
    }
    show_image();

    if(!other_data.empty())
    {

        std::vector<std::string> other_params(other_data.size());

        // generalized file names
        {
            std::string filename1 = std::filesystem::path(file_name.toStdString()).filename().string();
            std::string filename2 = std::filesystem::path(param1).filename().string();
            for(size_t i = 0;i < other_data.size();++i)
            {
                // if param1 is file name, then try to generalize
                if(param1.find('.') != std::string::npos)
                {
                    std::string filename1_others = std::filesystem::path(other_file_name[i]).filename().string();
                    // directory is the only difference
                    if(filename1 == filename2)
                        other_params[i] = QString(param1.c_str()).
                                replace(filename1.c_str(),QFileInfo(other_file_name[i].c_str()).fileName()).toStdString();
                    else
                    // common postfix
                    {
                        size_t common_prefix = 0;
                        for(;common_prefix < filename1.length() && common_prefix < filename2.length();++common_prefix)
                            if(filename1[common_prefix] != filename2[common_prefix])
                                break;
                        if(common_prefix == 0) // consider prefix different
                        {
                            QMessageBox::critical(this,"Error",QString("Cannot apply the same for ")+
                                                  QFileInfo(other_file_name[i].c_str()).fileName());
                            return false;
                        }
                        else
                        {
                            size_t postfix_length = filename1.length()-common_prefix;
                            other_params[i] = other_file_name[i].substr(0,other_file_name[i].length()-postfix_length)
                                                    + filename2.substr(common_prefix);
                        }

                        if(other_params[i] == other_file_name[i])
                        {
                            QMessageBox::critical(this,"Error",QString("Cannot apply the same for ")+
                                                  QFileInfo(other_file_name[i].c_str()).fileName());
                            return false;
                        }
                    }
                }
                else
                    other_params[i] = param1;
            }
        }

        begin_prog("applying to others");
        for(size_t i = 0;check_prog(i,other_data.size());++i)
        {
            if(!img_command(other_data[i],other_vs[i],other_T[i],cmd,other_params[i],param2,error_msg))
            {
                QMessageBox::critical(this,"Error",QString("Operation stopped at ")+
                                      QFileInfo(other_file_name[i].c_str()).fileName() + " error:" +
                                      error_msg.c_str());
                return false;
            }
        }
        QMessageBox::information(this,"DSI Studio","Operation applied to other images");
    }
    return true;
}


void show_view(QGraphicsScene& scene,QImage I);
bool load_image_from_files(QStringList filenames,tipl::image<float,3>& ref,tipl::vector<3>& vs,tipl::matrix<4,4,float>& trans)
{
    if(filenames.size() == 1 && filenames[0].toLower().contains("nii"))
    {
        gz_nifti in;
        if(!in.load_from_file(filenames[0].toLocal8Bit().begin()) || !in.toLPS(ref))
        {
            QMessageBox::information(nullptr,"Error","Not a valid nifti file");
            return false;
        }
        in.get_voxel_size(vs);
        in.get_image_transformation(trans);
        return true;
    }
    else
        if(filenames.size() == 1 && filenames[0].contains("2dseq"))
        {
            tipl::io::bruker_2dseq seq;
            if(!seq.load_from_file(filenames[0].toLocal8Bit().begin()))
            {
                QMessageBox::information(nullptr,"Error","Not a valid 2dseq file");
                return false;
            }
            seq.get_image().swap(ref);
            seq.get_voxel_size(vs);
            return true;
        }
    else
    {
        tipl::io::volume v;
        std::vector<std::string> file_list;
        for(int i = 0;i < filenames.size();++i)
            file_list.push_back(filenames[i].toStdString());
        v.load_from_files(file_list,file_list.size());
        v >> ref;
        v.get_voxel_size(vs);
        return true;
    }
}

view_image::view_image(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::view_image)
{
    ui->setupUi(this);
    ui->info->setColumnWidth(0,120);
    ui->info->setColumnWidth(1,200);
    ui->info->setHorizontalHeaderLabels(QStringList() << "Header" << "Value");
    ui->view->setScene(&source);
    ui->max_color->setColor(0xFFFFFFFF);
    ui->min_color->setColor(0XFF000000);
    ui->dwi_volume->hide();
    ui->dwi_label->hide();
    connect(ui->max_color,SIGNAL(clicked()),this,SLOT(change_contrast()));
    connect(ui->min_color,SIGNAL(clicked()),this,SLOT(change_contrast()));

    source_ratio = 2.0;
    ui->tabWidget->setCurrentIndex(0);


    qApp->installEventFilter(this);
}

void save_idx(const char* file_name,std::shared_ptr<gz_istream> in);
view_image::~view_image()
{
    save_idx(file_name.toStdString().c_str(),nifti.input_stream);
    qApp->removeEventFilter(this);
    delete ui;
}
bool view_image::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::Wheel && obj->parent() == ui->view)
    {
        QWheelEvent* we = dynamic_cast<QWheelEvent*>(event);
        if(!we)
            return false;
        if(we->delta() < 0)
            on_zoom_in_clicked();
        else
            on_zoom_out_clicked();
        event->accept();

        return true;
    }
    if (event->type() != QEvent::MouseMove || obj->parent() != ui->view)
        return false;

    QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
    QPointF point = ui->view->mapToScene(mouseEvent->pos().x(),mouseEvent->pos().y());
    tipl::vector<3,float> pos,mni;
    tipl::slice2space(cur_dim,
                      std::round(float(point.x()) / source_ratio),
                      std::round(float(point.y()) / source_ratio),ui->slice_pos->value(),pos[0],pos[1],pos[2]);
    if(!data.geometry().is_valid(pos))
        return true;
    mni = pos;
    mni.to(T);
    ui->info_label->setText(QString("(%1,%2,%3) MNI(%4,%5,%6) = %7").arg(pos[0]).arg(pos[1]).arg(pos[2])
                                                                    .arg(mni[0]).arg(mni[1]).arg(mni[2])
                                                                    .arg(data.at(pos[0],pos[1],pos[2])));
    return true;
}


bool get_compressed_image(tipl::io::dicom& dicom,tipl::image<short,2>& I)
{
    QByteArray array((char*)&*dicom.compressed_buf.begin(),dicom.buf_size);
    QBuffer qbuff(&array);
    QImageReader qimg;
    qimg.setDecideFormatFromContent(true);
    qimg.setDevice(&qbuff);
    QImage img;
    if(!qimg.read(&img))
    {
        std::cout << "Unsupported transfer syntax:" << dicom.encoding;
        return false;
    }
    QImage buf = img.convertToFormat(QImage::Format_RGB32);
    I.resize(tipl::geometry<2>(buf.width(),buf.height()));
    const uchar* ptr = buf.bits();
    for(int j = 0;j < I.size();++j,ptr += 4)
        I[j] = *ptr;
    return true;
}
void prepare_idx(const char* file_name,std::shared_ptr<gz_istream> in);
bool view_image::open(QStringList file_names)
{
    tipl::io::dicom dicom;
    tipl::io::bruker_2dseq seq;
    gz_mat_read mat;
    data.clear();
    T.identity();

    QString info;
    file_name = file_names[0];
    setWindowTitle(QFileInfo(file_name).fileName());
    prog_init p("loading ",std::filesystem::path(file_name.toStdString()).filename().string().c_str());
    check_prog(0,1);

    if(file_names.size() > 1 && QString(file_name).endsWith(".bmp"))
    {
        for(unsigned int i = 0;check_prog(i,file_names.size());++i)
        {
            tipl::color_image I;
            tipl::io::bitmap bmp;
            if(!bmp.load_from_file(file_names[i].toStdString().c_str()))
                return false;
            bmp >> I;
            if(i == 0)
                data.resize(tipl::geometry<3>(I.width(),I.height(),file_names.size()));
            unsigned int pos = i*I.size();
            for(unsigned int j = 0;j < I.size();++j)
                data[pos+j] = ((float)I[j].r+(float)I[j].r+(float)I[j].r)/3.0;
        }
    }
    else
    if(QString(file_name).endsWith(".nii.gz") || QString(file_name).endsWith(".nii"))
    {
        prepare_idx(file_name.toStdString().c_str(),nifti.input_stream);
        if(!nifti.load_from_file(file_name.toStdString().c_str()))
        {
            QMessageBox::critical(this,"Error","Invalid NIFTI file");
            return false;
        }
        if(nifti.dim(4) > 1)
        {
            ui->dwi_volume->setMaximum(nifti.dim(4)-1);
            dwi_volume_buf.resize(nifti.dim(4));
            nifti.input_stream->sample_access_point = true;
            ui->dwi_volume->show();
            ui->dwi_label->show();
        }
        nifti >> data;
        nifti.get_voxel_size(vs);
        nifti.get_image_transformation(T);
        info = QString("sizeof_hdr=%1\ndim_info=%2\n").
                arg(nifti.nif_header2.sizeof_hdr).
                arg((int)nifti.nif_header2.dim_info);
        for(unsigned int i = 0;i < 8;++i)
            info +=  QString("dim[%1]=%2\n").
                    arg(i).arg(nifti.nif_header2.dim[i]);
        info += QString("intent_p1=%1\n").arg(nifti.nif_header2.intent_p1);
        info += QString("intent_p2=%1\n").arg(nifti.nif_header2.intent_p2);
        info += QString("intent_p3=%1\n").arg(nifti.nif_header2.intent_p3);
        info += QString("intent_code=%1\n").arg(nifti.nif_header2.intent_code);
        info += QString("datatype=%1\n").arg(nifti.nif_header2.datatype);
        info += QString("bitpix=%1\n").arg(nifti.nif_header2.bitpix);
        info += QString("slice_start=%1\n").arg(nifti.nif_header2.slice_start);

        for(unsigned int i = 0;i < 8;++i)
            info +=  QString("pixdim[%1]=%2\n").
                    arg(i).arg(nifti.nif_header2.pixdim[i]);

        info += QString("vox_offset=%1\n").arg(nifti.nif_header2.vox_offset);
        info += QString("scl_slope=%1\n").arg(nifti.nif_header2.scl_slope);
        info += QString("scl_inter=%1\n").arg(nifti.nif_header2.scl_inter);
        info += QString("slice_end=%1\n").arg(nifti.nif_header2.slice_end);
        info += QString("slice_code=%1\n").arg((int)nifti.nif_header2.slice_code);
        info += QString("xyzt_units=%1\n").arg((int)nifti.nif_header2.xyzt_units);
        info += QString("scl_inter=%1\n").arg(nifti.nif_header2.scl_inter);
        info += QString("cal_max=%1\n").arg(nifti.nif_header2.cal_max);
        info += QString("cal_min=%1\n").arg(nifti.nif_header2.cal_min);
        info += QString("slice_duration=%1\n").arg(nifti.nif_header2.slice_duration);
        info += QString("toffset=%1\n").arg(nifti.nif_header2.toffset);
        info += QString("descrip=%1\n").arg(nifti.nif_header2.descrip);
        info += QString("aux_file=%1\n").arg(nifti.nif_header2.aux_file);
        info += QString("qform_code=%1\n").arg(nifti.nif_header2.qform_code);
        info += QString("sform_code=%1\n").arg(nifti.nif_header2.sform_code);
        info += QString("quatern_b=%1\n").arg(nifti.nif_header2.quatern_b);
        info += QString("quatern_c=%1\n").arg(nifti.nif_header2.quatern_c);
        info += QString("quatern_d=%1\n").arg(nifti.nif_header2.quatern_d);
        info += QString("qoffset_x=%1\n").arg(nifti.nif_header2.qoffset_x);
        info += QString("qoffset_y=%1\n").arg(nifti.nif_header2.qoffset_y);
        info += QString("qoffset_z=%1\n").arg(nifti.nif_header2.qoffset_z);

        for(unsigned int i = 0;i < 4;++i)
            info +=  QString("srow_x[%1]=%2\n").
                    arg(i).arg(nifti.nif_header2.srow_x[i]);
        for(unsigned int i = 0;i < 4;++i)
            info +=  QString("srow_y[%1]=%2\n").
                    arg(i).arg(nifti.nif_header2.srow_y[i]);
        for(unsigned int i = 0;i < 4;++i)
            info +=  QString("srow_z[%1]=%2\n").
                    arg(i).arg(nifti.nif_header2.srow_z[i]);

        info += QString("intent_name=%1\n").arg(nifti.nif_header2.intent_name);

        if(file_names.size() > 1)
        {
            begin_prog("reading");
            QString failed_list;
            for(int i = 1;check_prog(i,file_names.size());++i)
            {
                gz_nifti other_nifti;
                if(!other_nifti.load_from_file(file_names[i].toStdString()))
                {
                    if(!failed_list.isEmpty())
                        failed_list += ",";
                    failed_list += QFileInfo(file_names[i]).fileName();
                    continue;
                }
                tipl::image<float,3> odata;
                tipl::vector<3> ovs;
                tipl::matrix<4,4,float> oT;
                other_nifti >> odata;
                other_nifti.get_voxel_size(ovs);
                other_nifti.get_image_transformation(oT);
                other_file_name.push_back(file_names[i].toStdString());
                other_data.push_back(odata);
                other_vs.push_back(ovs);
                other_T.push_back(oT);
            }
            if(!failed_list.isEmpty())
                QMessageBox::critical(this,"Error",QString("Some files could not be opened:")+failed_list);
            else
                QMessageBox::information(this,"DSI Studio",QString("Other files read in memory for operation"));
        }
    }
    else
        if(dicom.load_from_file(file_name.toStdString()))
        {
            dicom >> data;
            if(dicom.is_compressed)
            {
                tipl::image<short,2> I;
                if(!get_compressed_image(dicom,I))
                {
                    QMessageBox::information(this,"Error",QString("Unsupported transfer syntax:") + QString(dicom.encoding.c_str()),0);
                    return false;
                }
                if(I.size() == data.size())
                    std::copy(I.begin(),I.end(),data.begin());
            }
            dicom.get_voxel_size(vs);
            std::string info_;
            dicom >> info_;

            if(dicom_dictionary.empty())
            {
                QFile data(":/data/dicom_tag.txt");
                if (data.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    QTextStream in(&data);
                    while (!in.atEnd())
                    {
                        QStringList list = in.readLine().split('\t');
                        if(list.size() < 3)
                            continue;
                        std::string value = list[2].toStdString();
                        std::replace(value.begin(),value.end(),' ','_');
                        dicom_dictionary[list[0].toStdString()] = value;
                    }
                }
            }
            std::ostringstream out;
            std::istringstream in(info_);
            std::string line;
            while(std::getline(in,line))
            {

                for(size_t pos = 0;(pos = line.find('(',pos)) != std::string::npos;++pos)
                {
                    std::string tag = line.substr(pos,11);
                    if(tag.length() != 11)
                        continue;
                    std::string tag2 = tag;
                    tag2[3] = 'x';
                    tag2[4] = 'x';
                    auto iter = dicom_dictionary.find(tag);
                    if(iter == dicom_dictionary.end())
                        iter = dicom_dictionary.find(tag2);
                    if(iter != dicom_dictionary.end())
                        line.replace(pos,11,tag+iter->second);
                }
                out << line << std::endl;
            }
            info_ = out.str();
            info = info_.c_str();
        }
        else
            if(mat.load_from_file(file_name.toLocal8Bit().begin()))
            {
                mat >> data;
                mat.get_voxel_size(vs);
                for(unsigned int index = 0;index < mat.size();++index)
                {
                    std::string data;
                    mat[index].get_info(data);
                    info += QString("%1 [%2x%3]=%4\n").arg(mat[index].get_name().c_str()).
                            arg(mat[index].get_rows()).
                            arg(mat[index].get_cols()).
                            arg(data.c_str());
                }
            }
            else
            if(seq.load_from_file(file_name.toLocal8Bit().begin()))
            {
                data = std::move(seq.get_image());
                seq.get_voxel_size(vs);
            }
    QStringList list = info.split("\n");
    ui->info->clear();
    ui->info->setRowCount(list.size());
    for(int row = 0;row < list.size();++row)
    {
        QString line = list[row];
        QStringList value_list = line.split("=");
        ui->info->setItem(row,0, new QTableWidgetItem(value_list[0]));
        if(value_list.size() > 1)
            ui->info->setItem(row,1, new QTableWidgetItem(value_list[1]));
    }
    ui->info->selectRow(0);

    if(!data.empty())
    {
        init_image();
        show_image();
    }
    return !data.empty() || !info.isEmpty();
}

void view_image::init_image(void)
{
    no_update = true;
    max_value = tipl::maximum(data);
    min_value = tipl::minimum(data);
    float range = max_value-min_value;
    QString dim_text = QString("%1,%2,%3").arg(data.width()).arg(data.height()).arg(data.depth());
    if(!dwi_volume_buf.empty())
        dim_text += QString(",%1").arg(dwi_volume_buf.size());
    ui->image_info->setText(QString("dim=(%1) vs=(%4,%5,%6) T=(%7,%8,%9,%10;%11,%12,%13,%14;%15,%16,%17,%18)").
            arg(dim_text).
            arg(double(vs[0])).arg(double(vs[1])).arg(double(vs[2])).
            arg(double(T[0])).arg(double(T[1])).arg(double(T[2])).arg(double(T[3])).
            arg(double(T[4])).arg(double(T[5])).arg(double(T[6])).arg(double(T[7])).
            arg(double(T[8])).arg(double(T[9])).arg(double(T[10])).arg(double(T[11])));
    ui->min->setRange(double(min_value-range/3),double(max_value));
    ui->max->setRange(double(min_value),double(max_value+range/3));
    ui->min->setSingleStep(double(range/20));
    ui->max->setSingleStep(double(range/20));
    ui->min->setValue(double(min_value));
    ui->max->setValue(double(max_value));
    slice_pos[0] = data.width()/2;
    slice_pos[1] = data.height()/2;
    slice_pos[2] = data.depth()/2;
    on_AxiView_clicked();
    no_update = false;
}
void view_image::show_image(void)
{
    if(data.empty() || no_update)
        return;
    tipl::image<float,2> buf;
    tipl::volume2slice(data, buf, cur_dim, size_t(slice_pos[cur_dim]));
    v2c.convert(buf,buffer);
    QImage I(reinterpret_cast<unsigned char*>(&*buffer.begin()),buffer.width(),buffer.height(),QImage::Format_RGB32);
    //if(cur_view != 2)
    //    I = I.mirrored(false,true);
    source_image = I.scaled(buffer.width()*source_ratio,buffer.height()*source_ratio);
    show_view(source,source_image);
}
void view_image::change_contrast()
{
    v2c.set_range(float(ui->min->value()),float(ui->max->value()));
    v2c.two_color(ui->min_color->color().rgb(),ui->max_color->color().rgb());
    show_image();
}
void view_image::on_zoom_in_clicked()
{
     source_ratio *= 1.1f;
     show_image();
}

void view_image::on_zoom_out_clicked()
{
    source_ratio *= 0.9f;
    show_image();
}

bool is_label_image(const tipl::image<float,3>& I);
void view_image::on_actionResample_triggered()
{
    bool ok;
    float nv = float(QInputDialog::getDouble(this,
        "DSI Studio","Assign output resolution in (mm):", double(vs[0]),0.0,3.0,4, &ok));
    if (!ok || nv == 0.0f)
        return;
    tipl::vector<3,float> new_vs(nv,nv,nv);
    tipl::image<float,3> J(tipl::geometry<3>(
            int(std::ceil(float(data.width())*vs[0]/new_vs[0])),
            int(std::ceil(float(data.height())*vs[1]/new_vs[1])),
            int(std::ceil(float(data.depth())*vs[2]/new_vs[2]))));
    if(J.empty())
        return;
    tipl::transformation_matrix<float> T1;
    tipl::matrix<4,4,float> nT;
    nT.identity();
    nT[0] = T1.sr[0] = new_vs[0]/vs[0];
    nT[5] = T1.sr[4] = new_vs[1]/vs[1];
    nT[10] = T1.sr[8] = new_vs[2]/vs[2];
    tipl::resample_mt(data,J,T1,is_label_image(data) ? tipl::nearest : tipl::cubic);
    data.swap(J);
    vs = new_vs;
    T = T*nT;

    init_image();
    show_image();
}
void view_image::on_actionSave_triggered()
{
    if(command("save",file_name.toStdString()))
        QMessageBox::information(this,"DSI Studio","Saved");
}

void view_image::on_action_Save_as_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save image",file_name,"NIFTI file(*nii.gz *.nii)" );
    if (filename.isEmpty())
        return;
    if(command("save",filename.toStdString()))
        QMessageBox::information(this,"DSI Studio","Saved");
    file_name = filename;
    setWindowTitle(QFileInfo(file_name).fileName());
}
void view_image::on_actionSave_as_Int8_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save image",file_name,"NIFTI file(*nii.gz *.nii)" );
    if (filename.isEmpty())
        return;
    tipl::image<uint8_t,3> new_data = data;
    gz_nifti nii;
    nii.set_image_transformation(T);
    nii.set_voxel_size(vs);
    nii << new_data;
    nii.save_to_file(filename.toStdString().c_str());
    file_name = filename;
    setWindowTitle(QFileInfo(file_name).fileName());
}

void view_image::on_actionSave_as_Int16_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save image",file_name,"NIFTI file(*nii.gz *.nii)" );
    if (filename.isEmpty())
        return;
    tipl::image<uint16_t,3> new_data = data;
    gz_nifti nii;
    nii.set_image_transformation(T);
    nii.set_voxel_size(vs);
    nii << new_data;
    nii.save_to_file(filename.toStdString().c_str());
    file_name = filename;
    setWindowTitle(QFileInfo(file_name).fileName());
}

void view_image::on_actionResize_triggered()
{
    std::ostringstream out;
    out << data.width() << " " << data.height() << " " << data.depth();
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign image dimension (width height depth)",QLineEdit::Normal,
                                           out.str().c_str(),&ok);

    if(!ok)
        return;
    std::istringstream in(result.toStdString());
    int w,h,d;
    in >> w >> h >> d;
    tipl::image<float,3> new_data(tipl::geometry<3>(w,h,d));
    tipl::draw(data,new_data,tipl::vector<3>());
    data.swap(new_data);
    init_image();
    show_image();
}

void view_image::on_actionTranslocate_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign image translocation (x y z)",QLineEdit::Normal,
                                           "0 0 0",&ok);

    if(!ok)
        return;
    std::istringstream in(result.toStdString());
    int dx,dy,dz;
    in >> dx >> dy >> dz;
    tipl::image<float,3> new_data(data.geometry());
    tipl::draw(data,new_data,tipl::vector<3>(dx,dy,dz));
    data.swap(new_data);
    T[3] -= T[0]*dx;
    T[7] -= T[5]*dy;
    T[11] -= T[10]*dz;
    init_image();
    show_image();
}

void view_image::on_actionTrim_triggered()
{
    tipl::vector<3,int> range_min,range_max;
    tipl::bounding_box(data,range_min,range_max,data[0]);
    int margin = 1;
    tipl::vector<3,int> translocate(margin,margin,0);
    range_min[2] += 1;
    range_max[2] -= 1;
    translocate -= range_min;
    range_max -= range_min;
    range_max[0] += margin+margin;
    range_max[1] += margin+margin;
    tipl::image<float,3> new_data(tipl::geometry<3>(range_max[0],range_max[1],range_max[2]));
    tipl::draw(data,new_data,translocate);
    data.swap(new_data);
    T[3] -= T[0]*translocate[0];
    T[7] -= T[5]*translocate[1];
    T[11] -= T[10]*translocate[2];
    init_image();
    show_image();
}

void view_image::on_actionSet_Translocation_triggered()
{
    std::ostringstream out;
    out << T[3] << " " << T[7] << " " << T[11];
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign the translocation vector translocation (x y z)",QLineEdit::Normal,
                                           out.str().c_str(),&ok);

    if(!ok)
        return;
    std::istringstream in(result.toStdString());
    in >> T[3] >> T[7] >> T[11];
    init_image();
    show_image();
}

void view_image::on_actionSet_Transformation_triggered()
{
    std::ostringstream out;
    for(int i = 0;i < 16;++i)
        out << T[i] << " ";
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign the transformation matrix",QLineEdit::Normal,
                                           out.str().c_str(),&ok);

    if(!ok)
        return;
    std::istringstream in(result.toStdString());
    for(int i = 0;i < 16;++i)
        in >> T[i];
    init_image();
    show_image();
}

void view_image::on_actionLower_threshold_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign lower threshold value ",QLineEdit::Normal,
                                           "0",&ok);
    if(!ok)
        return;
    float value = result.toFloat(&ok);
    if(!ok)
        return;
    tipl::lower_threshold(data,value);
    init_image();
    show_image();
}

void view_image::on_actionLPS_RAS_swap_triggered()
{
    if(data.empty())
        return;
    tipl::matrix<4,4,float> T2;
    T2.identity();
    T2[0] = -1.0f;
    T2[3] = data.width();
    T2[5] = -1.0f;
    T2[7] = data.height();
    T = T2*T;
    tipl::flip_x(data);
    tipl::flip_y(data);
    init_image();
    show_image();
}


void view_image::on_actionIntensity_shift_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign shift value",QLineEdit::Normal,
                                           "0",&ok);
    if(!ok)
        return;
    float value = result.toFloat(&ok);
    if(!ok)
        return;
    tipl::add_constant(data,value);
    init_image();
    show_image();
}

void view_image::on_actionIntensity_scale_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign scale value",QLineEdit::Normal,
                                           "0",&ok);
    if(!ok)
        return;
    float value = result.toFloat(&ok);
    if(!ok)
        return;
    tipl::multiply_constant(data,value);
    init_image();
    show_image();
}

void view_image::on_actionUpper_Threshold_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign upper threshold value ",QLineEdit::Normal,
                                           "0",&ok);
    if(!ok)
        return;
    float value = result.toFloat(&ok);
    if(!ok)
        return;
    tipl::upper_threshold(data,value);
    init_image();
    show_image();
}
bool is_label_image(const tipl::image<float,3>& I);
void view_image::on_actionSmoothing_triggered()
{
    tipl::image<float,3> new_data(data.geometry());
    uint32_t m = uint32_t(*std::max_element(data.begin(),data.end()));
    tipl::image<char,3> all_mask(data.geometry());
    for(size_t i = 0;i < data.size();++i)
        if(data[i] > 0.0f)
            all_mask[i] = 1;
    tipl::morphology::smoothing(all_mask);

    // smooth each region
    tipl::par_for(m,[&](uint32_t index)
    {
        tipl::image<char,3> mask(data.geometry());
        for(size_t i = 0;i < data.size();++i)
            if(uint32_t(data[i]) == index)
                mask[i] = 1;
        tipl::morphology::smoothing(mask);
        float value = float(index);
        for(size_t i = 0;i < data.size();++i)
            if(mask[i])
                new_data[i] = value;
    });

    // fill up gaps
    tipl::par_for(m,[&](uint32_t index)
    {
        tipl::image<char,3> mask(data.geometry());
        for(size_t i = 0;i < new_data.size();++i)
            if(uint32_t(new_data[i]) == index)
                mask[i] = 1;
        tipl::morphology::dilation(mask);
        float value = float(index);
        for(size_t i = 0;i < data.size();++i)
            if(new_data[i] == 0.0f && mask[i])
                new_data[i] = value;
    });

    new_data.swap(data);
    init_image();
    show_image();
}


void view_image::on_actionNormalize_Intensity_triggered()
{
    tipl::normalize(data,1.0f);
    init_image();
    show_image();
}

void view_image::on_min_slider_sliderMoved(int)
{
    ui->min->setValue(ui->min->minimum()+(ui->min->maximum()-ui->min->minimum())*
                      double(ui->min_slider->value())/double(ui->min_slider->maximum()));
}

void view_image::on_min_valueChanged(double)
{
    ui->min_slider->setValue(int((ui->min->value()-ui->min->minimum())*double(ui->min_slider->maximum())/
                             (ui->min->maximum()-ui->min->minimum())));
    change_contrast();
}

void view_image::on_max_slider_sliderMoved(int)
{
    ui->max->setValue(ui->max->minimum()+(ui->max->maximum()-ui->max->minimum())*
                      double(ui->max_slider->value())/double(ui->max_slider->maximum()));
}

void view_image::on_max_valueChanged(double)
{
    ui->max_slider->setValue(int((ui->max->value()-ui->max->minimum())*double(ui->max_slider->maximum())/
                             (ui->max->maximum()-ui->max->minimum())));
    change_contrast();
}

void view_image::on_AxiView_clicked()
{
    cur_dim = 2;
    ui->slice_pos->setRange(0,data.depth()-1);
    ui->slice_pos->setValue(slice_pos[cur_dim]);
}

void view_image::on_CorView_clicked()
{
    cur_dim = 1;
    ui->slice_pos->setRange(0,data.height()-1);
    ui->slice_pos->setValue(slice_pos[cur_dim]);
}

void view_image::on_SagView_clicked()
{
    cur_dim = 0;
    ui->slice_pos->setRange(0,data.width()-1);
    ui->slice_pos->setValue(slice_pos[cur_dim]);
}

void view_image::on_slice_pos_valueChanged(int value)
{
    if(data.empty())
        return;
    slice_pos[cur_dim] = value;
    show_image();
}

void view_image::on_actionSobel_triggered()
{
    tipl::filter::sobel(data);
    init_image();
    show_image();
}

void view_image::on_actionMorphology_triggered()
{
    tipl::morphology::edge(data);
    init_image();
    show_image();
}

void view_image::on_actionMorphology_Thin_triggered()
{
    tipl::morphology::edge_thin(data);
    init_image();
    show_image();
}

void view_image::on_actionMorphology_XY_triggered()
{
    tipl::morphology::edge_xy(data);
    init_image();
    show_image();
}

void view_image::on_actionMorphology_XZ_triggered()
{
    tipl::morphology::edge_xz(data);
    init_image();
    show_image();
}


void view_image::on_actionImageAddition_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
                           this,"Open other another image to apply",QFileInfo(file_name).absolutePath(),"NIFTI file(*nii.gz *.nii)" );
    if (filename.isEmpty())
        return;
    command("image_addition",filename.toStdString(),std::string());
}

void view_image::on_actionImageMultiplication_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
                           this,"Open other another image to apply",QFileInfo(file_name).absolutePath(),"NIFTI file(*nii.gz *.nii)" );
    if (filename.isEmpty())
        return;
    command("image_multiplication",filename.toStdString(),std::string());
}

void view_image::on_actionSignal_Smoothing_triggered()
{
    tipl::filter::mean(data);
    init_image();
    show_image();
}

void view_image::on_dwi_volume_valueChanged(int value)
{
    dwi_volume_buf[cur_dwi_volume].swap(data); // return image data to buffer

    cur_dwi_volume = size_t(value);
    if(dwi_volume_buf[cur_dwi_volume].empty())
    {
        has_gui = false;
        nifti.select_volume(cur_dwi_volume);
        tipl::image<float,3> new_data;
        nifti >> new_data;
        has_gui = true;
        if(new_data.empty())
            return;
        new_data.swap(data);
    }
    else
        dwi_volume_buf[cur_dwi_volume].swap(data);

    ui->dwi_label->setText(QString("DWI(%1/%2)").arg(value+1).arg(ui->dwi_volume->maximum()+1));
    init_image();
    show_image();
}

void view_image::on_actionDownsample_by_2_triggered()
{
    tipl::downsample_with_padding2(data);
    init_image();
    show_image();
}

void view_image::on_actionUpsample_by_2_triggered()
{
    tipl::upsampling(data);
    init_image();
    show_image();
}
